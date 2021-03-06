#define _XOPEN_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "elfloader.h"
#include "protmem.h"
#include "util.h"
#include "debug.h"
#include "config.h"

#ifdef __PS4__
#include <kernel.h>
#endif

#if defined(BinaryLoader) && !defined(__PS4__)
	#error BinaryLoader can not be build on x86-64
#endif
#if defined(BinaryLoader) && defined(ElfLoaderStandardIORedirectLazy)
	#error BinaryLoader does not support lazy io
#endif

/* Types */

typedef int (*Runnable)(int, char **);

typedef struct MainAndMemory
{
	Runnable main;
	ProtectedMemory *memory;
}
MainAndMemory;

/* Constants */

enum{ StandardIOServerPort = 5052 };
enum{ ServerPort = 5053 }; //hex(P) + hex(S)
enum{ ServerRetry = 20 };
enum{ ServerTimeout = 1 };
enum{ ServerBacklog = 10 };

/* Globals */

static ElfLoaderConfig *config;
static volatile int ioServer;
static pthread_t ioThread;

/* Standard IO globals for PS4 */

#ifdef __PS4__
FILE *__stdinp;
FILE **__stdinp_addr;
FILE *__stdoutp;
FILE **__stdoutp_addr;
FILE *__stderrp;
FILE **__stderrp_addr;
int __isthreaded;
int *__isthreaded_addr;
#endif

/* Binary loader (backwards compatibility) synced but rarely tested */

#ifdef BinaryLoader
int main(int argc, char **argv)
#else
int binaryLoaderMain(int argc, char **argv)
#endif
{
	int server, client;
	uint8_t *payload = (uint8_t *)Payload;
	ssize_t r;

	int stdfd[3];
	fpos_t stdpos[3];
	struct sigaction sa;

	#ifdef __PS4__
	int libc = sceKernelLoadStartModule("libSceLibcInternal.sprx", 0, NULL, 0, 0, 0);
	sceKernelDlsym(libc, "__stdinp", (void **)&__stdinp_addr);
	sceKernelDlsym(libc, "__stdoutp", (void **)&__stdoutp_addr);
	sceKernelDlsym(libc, "__stderrp", (void **)&__stderrp_addr);
	sceKernelDlsym(libc, "__isthreaded", (void **)&__isthreaded_addr);
	__stdinp = *__stdinp_addr;
	__stdoutp = *__stdoutp_addr;
	__stderrp = *__stderrp_addr;
	__isthreaded = *__isthreaded_addr;
	#endif

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigaction(SIGPIPE, &sa, 0);
	setvbuf(stdin, NULL, _IOLBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	config = malloc(sizeof(ElfLoaderConfig));
	configFromDefines(config);

	if(config->debugMode == DebugOn)
		debugEnable();

	if(config->standardIORedirectMode == StandardIORedirectWait)
	{
		int debug = utilSingleAcceptServer(StandardIOServerPort);
		utilStandardIORedirect(debug, stdfd, stdpos);
		close(debug);
	}

	debugPrint("debugOpen(%i)\n", StandardIOServerPort);

	debugPrint("Mode -> BinaryLoader\n");

	debugPrint("utilServerCreate(%i, %i, %i) -> ", ServerPort, ServerRetry, ServerTimeout);
	if((server = utilServerCreate(ServerPort, ServerBacklog, ServerRetry, ServerTimeout)) < 0)
	{
		debugPrint("Could not create server (return: %i)", server);
		return EXIT_FAILURE;
	}
	debugPrint("%i\n", server);

	debugPrint("accept(%i, NULL, NULL) -> ", server);
	if((client = accept(server, NULL, NULL)) < 0)
	{
		debugPrint("Could not accept client (return: %i)", client);
		close(server);
		return EXIT_FAILURE;
	}
	debugPrint("%i\n", client);

	while((r = read(client, payload, 4096)) > 0)
	{
		debugPrint("Read %"PRIi64" (0x%"PRIx64") bytes to %p \n", (int64_t)r, (uint64_t)r, (void *)payload);
		payload += r;
	}

	debugPrint("close(%i) -> ", client);
	debugPrint("%i\n", close(client));
	debugPrint("close(%i) -> ", server);
	debugPrint("%i\n", close(client));

	debugPrint("Executing binary at %p", (void *)payload);

	debugPrint("debugClose()\n");

	if(config->debugMode == DebugOn || config->standardIORedirectMode == StandardIORedirectWait)
		utilStandardIOReset(stdfd, stdpos);

	free(config);

	return EXIT_SUCCESS;
}

/* Standard IO server */

void *elfLoaderStandardIOServerThread(void *arg)
{
	int reset = 0;
	int client;
	int stdfd[3];
	fpos_t stdpos[3];

 	if((ioServer = utilServerCreate(StandardIOServerPort, 10, 20, 1)) < 0)
		return NULL;

	// FIXME: 0,1,2 would be trouble
	while(ioServer >= 0)
	{
		client = accept(ioServer, NULL, NULL);

		if(reset)
			utilStandardIOReset(stdfd, stdpos);
		reset = 1;

		if(client < 0 || ioServer < 0)
			continue;

		utilStandardIORedirect(client, stdfd, stdpos);
		close(client);
	}

	return NULL;
}

/* elf utils */

Elf *elfCreateFromSocket(int client)
{
	Elf *elf;
	size_t s;

	void *m = utilAllocUnsizeableFileFromDescriptor(client, &s);
	if(m == NULL)
		return NULL;
	elf = elfCreate(m, s);
	if(!elfLoaderIsLoadable(elf))
	{
		elfDestroyAndFree(elf);
		elf = NULL;
	}

	return elf;
}

Elf *elfCreateFromFile(char *path)
{
	Elf *elf;
	size_t s;

	void * m = utilAllocFile(path, &s);
	if(m == NULL)
		return NULL;
	elf = elfCreate(m, s);
	if(!elfLoaderIsLoadable(elf))
	{
		elfDestroyAndFree(elf);
		elf = NULL;
	}

	return elf;
}

/* elf loader surface (view) wrappers*/

Elf *elfLoaderServerAcceptElf(int server)
{
	int client;
	Elf *elf;

	if(server < 0)
	{
		debugPrint("Server is not a file descriptor");
		return NULL;
	}

	debugPrint("accept(%i, NULL, NULL) -> ", server);
	if((client = accept(server, NULL, NULL)) < 0)
	{
		close(server);
		debugPrint("Accept failed %i", client);
		return NULL;
	}
	debugPrint("%i\n", client);

	debugPrint("elfCreateFromSocket(%i) -> ", client);
	elf = elfCreateFromSocket(client);

	if(elf == NULL)
		debugPrint("File could not be read or doesn't seem to be an ELF\n");
	else
		debugPrint("%p\n", (void *)elf);

	debugPrint("close(%i) -> ", client);
	debugPrint("%i\n", close(client));

	return elf;
}

Elf *elfLoaderCreateElfFromPath(char *file)
{
	Elf *elf;

	if(file == NULL)
	{
		debugPrint("File path is NULL");
		return NULL;
	}

	debugPrint("elfCreateFromFile(%s) -> ", file);
	if((elf = elfCreateFromFile(file)) == NULL)
		debugPrint("File could not be read or doesn't seem to be an ELF\n");
	else
		debugPrint("%p\n", (void *)elf);

	return elf;
}

ProtectedMemory *elfLoaderMemoryCreate(Elf *elf)
{
	size_t size;
	ProtectedMemory *memory;

	if(elf == NULL)
	{
		debugPrint("Elf is NULL");
		return NULL;
	}

	size = elfMemorySize(elf);

	debugPrint("protectedMemoryCreate(%zu) -> ", size);

	if(config->memoryMode == MemoryEmulate)
		memory = protectedMemoryCreateEmulation(size);
	else
		memory = protectedMemoryCreate(size);

	if(memory == NULL)
		debugPrint("Memory Setup failed\n");
	else
		debugPrint("%p\n", (void *)memory);

	return memory;
}

int elfLoaderMemoryDestroySilent(ProtectedMemory *memory)
{
	int r;

	if(memory == NULL)
		return -1;

	if(config->memoryMode == MemoryEmulate)
		r = protectedMemoryDestroyEmulation(memory);
	else
		r = protectedMemoryDestroy(memory);

	return r;
}

int elfLoaderMemoryDestroy(ProtectedMemory *memory)
{
	int r;

	if(memory == NULL)
	{
		debugPrint("Memory is NULL");
		return -1;
	}

	debugPrint("protectedMemoryDestroy(%p) -> ", (void *)memory);
	r = elfLoaderMemoryDestroySilent(memory);

	if(r < 0)
		debugPrint("Memory could not be completely freed - ");
	debugPrint("%i\n", r);

	return r;
}

Runnable elfLoaderRunSetup(Elf *elf, ProtectedMemory *memory)
{
	Runnable run;
	int r;

	if(elf == NULL || memory == NULL)
	{
		debugPrint("Elf (%p)  or memory (%p) NULL\n", (void *)elf, (void *)memory);
		return NULL;
	}

	// FIXME: depricate for 3 method calls
	debugPrint("elfLoaderLoad(%p, %p, %p) -> ", (void *)elf, protectedMemoryWritable(memory), protectedMemoryExecutable(memory));
	run = NULL;
	if((r = elfLoaderLoad(elf, protectedMemoryWritable(memory), protectedMemoryExecutable(memory))) < 0)
		debugPrint("Elf could not be loaded - %i\n", r);
	else
	{
		debugPrint("%i\n", r);
		run = (Runnable)((uint8_t *)protectedMemoryExecutable(memory) + elfEntry(elf));
	}

	debugPrint("elfDestroyAndFree(%p)\n", (void *)elf);
	elfDestroyAndFree(elf); // we don't need the "file" anymore

	return run;
}

void elfLoaderRunSync(Elf *elf)
{
	ProtectedMemory *memory;
	Runnable run;
	int r;

	char *elfName = "elf";
	char *elfArgv[2] = { elfName, NULL };
	int elfArgc = sizeof(elfArgv) / sizeof(elfArgv[0]) - 1;

	if(elf == NULL)
	{
		debugPrint("Elf is NULL");
		return;
	}

	memory = elfLoaderMemoryCreate(elf);
	run = elfLoaderRunSetup(elf, memory);

	if(run != NULL)
	{
		debugPrint("run(%i, {\"%s\", NULL}) [%p + elfEntry = %p] -> ", elfArgc, elfArgv[0], protectedMemoryExecutable(memory), (void *)run);
		r = run(elfArgc, elfArgv);
		debugPrint("%i\n", r);
	}

	elfLoaderMemoryDestroy(memory);
}

void *elfLoaderRunAsyncMain(void *mainAndMemory)
{
	MainAndMemory *mm;
	int r;

	char *elfName = "elf";
	char *elfArgv[2] = { elfName, NULL };
	int elfArgc = sizeof(elfArgv) / sizeof(elfArgv[0]) - 1;

	if(mainAndMemory == NULL)
	{
		debugPrint("mainAndMemory is NULL");
		return NULL;
	}

	mm = (MainAndMemory *)mainAndMemory;
	r = mm->main(elfArgc, elfArgv);
	debugPrint("Asynchonous Return %i\n", r);
	elfLoaderMemoryDestroySilent(mm->memory);
	free(mm);

	return NULL;
}

void elfLoaderRunAsync(Elf *elf)
{
	pthread_t thread;
	MainAndMemory *mm;

	if(elf == NULL)
	{
		debugPrint("Elf is NULL");
		return;
	}

 	mm = (MainAndMemory *)malloc(sizeof(MainAndMemory));
	if(mm ==  NULL)
	{
		debugPrint("MainAndMemory allocation failed\n");
		debugPrint("elfDestroyAndFree(%p)\n", (void *)elf);
		elfDestroyAndFree(elf);
		return;
	}

	mm->memory = elfLoaderMemoryCreate(elf);
	mm->main = elfLoaderRunSetup(elf, mm->memory);

	if(mm->main != NULL)
	{
		debugPrint("run [%p + elfEntry = %p]\n", protectedMemoryExecutable(mm->memory), (void *)mm->main);
		pthread_create(&thread, NULL, elfLoaderRunAsyncMain, mm);
	}
	else
		free(mm);
}

#ifdef BinaryLoader
int elfLoaderMain(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	int server; // only used in ElfInputServer
	Elf *elf;
	int stdfd[3];
	fpos_t stdpos[3];
	struct sigaction sa;

	#ifdef __PS4__
	int libc = sceKernelLoadStartModule("libSceLibcInternal.sprx", 0, NULL, 0, 0, 0);
	sceKernelDlsym(libc, "__stdinp", (void **)&__stdinp_addr);
	sceKernelDlsym(libc, "__stdoutp", (void **)&__stdoutp_addr);
	sceKernelDlsym(libc, "__stderrp", (void **)&__stderrp_addr);
	sceKernelDlsym(libc, "__isthreaded", (void **)&__isthreaded_addr);
	__stdinp = *__stdinp_addr;
	__stdoutp = *__stdoutp_addr;
	__stderrp = *__stderrp_addr;
	__isthreaded = *__isthreaded_addr;
	#endif

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigaction(SIGPIPE, &sa, 0);
	setvbuf(stdin, NULL, _IOLBF, 0);
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	config = malloc(sizeof(ElfLoaderConfig));
	configFromDefines(config);

	#ifndef __PS4__
		configFromArguments(config, argc, argv);
 		if(config->elfInputMode == ElfInputFile && config->inputFile == NULL)
		{
			fprintf(stderr, "No file provided as argument in file mode.\n");
			free(config);
			return EXIT_FAILURE;
		}
	#endif

	if(config->debugMode == DebugOn)
		debugEnable();

	if(config->elfInputMode == ElfInputFile)
	{
		debugPrint("debugOpen(STDERR_FILENO)\n");

		debugPrint("Mode -> ElfLoader [input: %i, memory: %i, thread: %i, debug: %i, stdio: %i]\n",
			config->elfInputMode, config->memoryMode, config->threadingMode, config->debugMode, config->standardIORedirectMode);

		elf = elfLoaderCreateElfFromPath(config->inputFile);
		if(elf != NULL)
			elfLoaderRunSync(elf);
	}
	else // if(elfInputMode == ElfInputServer)
	{
		if(config->standardIORedirectMode == StandardIORedirectWait)
		{
			int debug = utilSingleAcceptServer(StandardIOServerPort);
			utilStandardIORedirect(debug, stdfd, stdpos);
			close(debug);
		}
		else if(config->standardIORedirectMode == StandardIORedirectLazy)
			pthread_create(&ioThread, NULL, elfLoaderStandardIOServerThread, NULL);

		debugPrint("debugOpen(%i)\n", StandardIOServerPort);

		debugPrint("Mode -> ElfLoader [input: %i, memory: %i, thread: %i, debug: %i, stdio: %i]\n",
			config->elfInputMode, config->memoryMode, config->threadingMode, config->debugMode, config->standardIORedirectMode);

		debugPrint("utilServerCreate(%i, %i, %i) -> ", ServerPort, ServerRetry, ServerTimeout);
		if((server = utilServerCreate(ServerPort, ServerRetry, ServerTimeout, ServerBacklog)) < 0)
			debugPrint("Server creation failed %i", server);
		else
			debugPrint("%i\n", server);

		if(server > 0)
		{
			if(config->threadingMode == ThreadingNone)
			{
				elf = elfLoaderServerAcceptElf(server);
				debugPrint("close(%i) -> ", server);
				debugPrint("%i\n", close(server));
				elfLoaderRunSync(elf);
			}
			else
			{
				while(1)
				{
					elf = elfLoaderServerAcceptElf(server);
	 				if(elf == NULL) // to stop, send a non-elf file - cheesy I know
						break;
					elfLoaderRunAsync(elf);
				}
				debugPrint("close(%i) -> ", server);
				debugPrint("%i\n", close(server));
			}
		}
	}

	debugPrint("debugClose()\n");

	if(config->elfInputMode == ElfInputServer)
	{
		if(config->standardIORedirectMode == StandardIORedirectWait)
			utilStandardIOReset(stdfd, stdpos);
		else if(config->standardIORedirectMode == StandardIORedirectLazy)
		{
			int s = ioServer;
			ioServer = -1;
			shutdown(s, SHUT_RDWR);
			close(s);
			pthread_join(ioThread, NULL);
		}
	}

	free(config);

	return EXIT_SUCCESS;
}
