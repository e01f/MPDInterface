#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <thread>
#include "../nde/NDE.h"
#include "../Winamp/wa_ipc.h"
#include "WINAMPCMD.H"
#include <iostream>
#include <time.h>
#include <shlwapi.h>
#pragma comment(lib,"shlwapi.lib")
#include "shlobj.h"

#pragma comment (lib, "Ws2_32.lib")
#define WIN32_LEAN_AND_MEAN

#define PORT					6600
#define	IN_BUFFER_SIZE			256
#define MAX_CONNECTIONS			15

#define F(s) s, strlen(s)
#define FW(s) (char *)s, wcslen(s)*sizeof(wchar_t)

enum protocolStates {
	RESET,
	READLINE,
	ERROR_BUFF_OVERFLOW,
	ERROR_MALFORMED_INPUT,
	PARSE_START,
	PARSE_KEYWORD,
	PARSE_CLEAR,
	PARSE_CONSUME,
	PARSE_CONSUME_ARG1,
	PARSE_STATUS,
	PARSE_PLAYLISTINFO,
	PARSE_PAUSE,
	PARSE_SEARCH,
	PARSE_SEARCH_ARG1,
	PARSE_SEARCH_DELIMITER1,
	PARSE_SEARCH_ARG2,
	PARSE_SEARCH_DELIMITER2,
	PARSE_SEARCH_ARG3,
	PARSE_SEARCH_ARG1_EX,
	PARSE_SEARCH_WAIT,
	PARSE_SEARCH_EXECUTE,
	PARSE_ADD,
	PARSE_ADD_ARG1,
	PARSE_ADD_DELIMITER1,
	PARSE_ADD_ARG2,
	PARSE_PLAY,
	PARSE_NEXT,
	PS_NUM // <-- always last, acts as counter...
};
typedef enum protocolStates protocolState;

enum argumentTypes {
	INTEGER,
	STRING
};
typedef enum argumentTypes argumentType;

struct argument_s {
	char *argValue; //= NULL;
	int argLen; //= 0;
	argumentType argType; //= INTEGER;
};
typedef struct argument_s argument_t;

struct clientThreadData_s {
	std::thread *clientThread;
	int clisockfd;
	char lineBuf[IN_BUFFER_SIZE];
	int lineBufSz; // = 0;
	int parserOffset; // = 0;
	protocolState protoState; // = READLINE;
	int argCount; // = 0;
	argument_t *args;
	bool isTerminated = false;
	Table *table;
	Scanner *scanner;
	unsigned long now;
	unsigned long	waitStamp;
	unsigned long	timerStamps[PS_NUM];
};
typedef struct clientThreadData_s clientThreadData_t;

HWND hWinamp;
volatile bool isRunning = true;
int emConsume = 0;
TCHAR szWAADIdxPath[MAX_PATH]; // i.e. C:/Users/Alexander/AppData/Roaming/Winamp/Plugins/ml/main.idx
TCHAR szWAADDatPath[MAX_PATH]; // i.e. C:/Users/Alexander/AppData/Roaming/Winamp/Plugins/ml/main.dat
Database db = NULL;

void pyUnescapeInPlace(char *str, int strLen) {
	// asbdhja\\asdasd\'\asd\asd\sss\\asd
	int skip = 0;
	for (int i = 0; i + skip < strLen; i++) {
		if (str[i + skip] == '\\') {
			switch (str[i + skip + 1]) {
				// Literal, so just remove the slashes
				case '"':
				case '\'':
				case '\\':
					skip++;
					break;
			default:
				// Not recognized, leave as it is
				break;
			}
		}
		str[i] = str[i + skip];
	}
}

int clear(clientThreadData_t &clientData) {
	// Clears the current playlist
	SendMessage(hWinamp, WM_WA_IPC, 0, IPC_DELETE);
	return 0;
}

int consume(clientThreadData_t &clientData) {
	
	if (clientData.argCount != 1) return 1;
	if (clientData.args[0].argType != INTEGER) return 2;
	if (clientData.args[0].argLen <= 0) return 3;

	emConsume = (clientData.args[0].argValue != 0) ? 1 : 0; // As per the common definition of boolean

	// Sets consume state to STATE, STATE should be 0 or 1. When consume is activated, each song played is removed from playlist.
	// Winamp does not seem to be able to do this, so lets emulate...
	return 0;
}

int status(clientThreadData_t &clientData) {
	// Reports the current status of the player and the volume level. ... quite a lot
	char buff[1024];
	int plState = SendMessage(hWinamp, WM_WA_IPC, 0, IPC_ISPLAYING);
	char plStateTxt[16];
	switch (plState) {
	case 0:
		sprintf(plStateTxt, "stop");
		break;
	case 3:
		sprintf(plStateTxt, "pause");
		break;
	case 1:
		sprintf(plStateTxt, "play");
		break;
	default:
		sprintf(plStateTxt, "unknown");
	}
	int listPos = SendMessage(hWinamp, WM_WA_IPC, 0, IPC_GETLISTPOS);
	long timePosMs = SendMessage(hWinamp, WM_WA_IPC, 0, IPC_GETOUTPUTTIME);
	long timeLenMs = SendMessage(hWinamp, WM_WA_IPC, 2, IPC_GETOUTPUTTIME);
	// TODO: With consume, this should trim the indexes / playlist length accordingly so as to make the illusion perfect
	sprintf(buff, "volume: %d\nrepeat: %d\nrandom: %d\nsingle: 0\nconsume: %d\nplaylist: 0\nplaylistlength: %d\nstate: %s\nsong: %d\nsongid: 0\nnextsong: %d\nnextsongid: 0\ntime: %d\nelapsed: %d\nduration: %d\nbitrate: 0\nxfade: 0\nmixrampdb: 0\nmixrampdelay: 0\naudio: nA\nupdating_db: 0\nerror: 0\n", IPC_GETVOLUME(hWinamp), SendMessage(hWinamp, WM_WA_IPC, 0, IPC_GET_REPEAT), SendMessage(hWinamp, WM_WA_IPC, 0, IPC_GET_SHUFFLE), emConsume, SendMessage(hWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH), plStateTxt, listPos, listPos + 1, (int)timePosMs / 1000, (int)timePosMs / 1000, (int)timeLenMs / 1000);
	send(clientData.clisockfd, F(buff), 0);
	return 0;
}

int ReadMemoryInt(HANDLE processHandle, LPCVOID address) {
	int buffer = 0;
	SIZE_T NumberOfBytesToRead = sizeof(buffer);
	SIZE_T NumberOfBytesActuallyRead;
	BOOL err = ReadProcessMemory(processHandle, address, &buffer, NumberOfBytesToRead, &NumberOfBytesActuallyRead);
	if (err || NumberOfBytesActuallyRead != NumberOfBytesToRead)
		return -1;
	return buffer;
}

char *ReadMemoryStr(HANDLE processHandle, LPCVOID address) {
	char *adr;
	adr = (char*)address;
	char *buffer = (char*) malloc(1 * sizeof(char));
	int buffidx = 0;
	SIZE_T NumberOfBytesToRead = sizeof(char);
	SIZE_T NumberOfBytesActuallyRead;
	do {
		BOOL err = ReadProcessMemory(processHandle, adr, &buffer[buffidx], NumberOfBytesToRead, &NumberOfBytesActuallyRead);
		if (!err || NumberOfBytesActuallyRead != NumberOfBytesToRead) {
			printf("ERROR: error %d while reading foreign process memory.\r\n", GetLastError());
			return NULL;
		}
		buffidx++;
		adr++;
		buffer = (char *) realloc(buffer, (buffidx + 1) * sizeof(char));
	} while (buffer[buffidx - 1] != '\0' && buffidx < 2048);

	return buffer;
}

wchar_t *ReadMemoryStrW(HANDLE processHandle, LPCVOID address) {
	wchar_t *adr;
	adr = (wchar_t*)address;
	wchar_t *buffer = (wchar_t*)malloc(1 * sizeof(wchar_t));
	int buffidx = 0;
	SIZE_T NumberOfBytesToRead = sizeof(wchar_t);
	SIZE_T NumberOfBytesActuallyRead;
	do {
		BOOL err = ReadProcessMemory(processHandle, adr, &buffer[buffidx], NumberOfBytesToRead, &NumberOfBytesActuallyRead);
		if (!err || NumberOfBytesActuallyRead != NumberOfBytesToRead) {
			printf("ERROR: error %d while reading foreign process memory.\r\n", GetLastError());
			return NULL;
		}
		buffidx++;
		adr++;
		buffer = (wchar_t *)realloc(buffer, (buffidx + 1) * sizeof(wchar_t));
	} while (buffer[buffidx - 1] != (wchar_t)'\0' && buffidx < 2048);

	return buffer;
}

void utf16ToUtf8(wchar_t *ut16, char *ut8) {
	int size_needed = WideCharToMultiByte(CP_UTF8, 0, ut16, -1, NULL, 0, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, ut16, -1, ut8, size_needed, NULL, NULL);
}

void utf8ToUtf16(char *ut8, wchar_t *ut16) {
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, ut8, -1, NULL, 0);
	MultiByteToWideChar(CP_UTF8, 0, ut8, -1, ut16, size_needed);
}

int playlistinfo(clientThreadData_t &clientData) {
	int listLen = SendMessage(hWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
	int listPos = SendMessage(hWinamp, WM_WA_IPC, 0, IPC_GETLISTPOS);

	if (listLen <= 0) return 0; // List is empty?

	DWORD dwWinampPID;
	GetWindowThreadProcessId(hWinamp, &dwWinampPID);
	HANDLE pWinamp;
	pWinamp = OpenProcess(PROCESS_ALL_ACCESS, 0, dwWinampPID);

	if (listLen > 1000) return 1;
	char buff[1024];
	for (int i = (emConsume == 0 ? 0 : listPos); i < listLen; i++) {

		char mbuff[MAX_PATH];
		wchar_t * fileN = ReadMemoryStrW(pWinamp, (wchar_t *)SendMessage(hWinamp, WM_WA_IPC, i, IPC_GETPLAYLISTFILEW));
		utf16ToUtf8(fileN, mbuff);
		
		DWORD dwWinampPID;
		GetWindowThreadProcessId(hWinamp, &dwWinampPID);
		HANDLE pWinamp;
		pWinamp = OpenProcess(PROCESS_ALL_ACCESS, 0, dwWinampPID);

		char *W_bFIS = (char *)VirtualAllocEx(pWinamp, NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
		wchar_t *W_bFISFileName = (wchar_t*)(W_bFIS + 1024);

		basicFileInfoStructW bFIS;
		bFIS.filename = W_bFISFileName;
		bFIS.quickCheck = 0;
		bFIS.titlelen = 0;
		bFIS.title = NULL;
		bFIS.length = 0;

		WriteProcessMemory(pWinamp, W_bFIS, &bFIS, sizeof(bFIS), NULL);
		WriteProcessMemory(pWinamp, W_bFISFileName, fileN, sizeof(wchar_t)*(wcslen(fileN) + 1), NULL);

		SendMessage(hWinamp, WM_WA_IPC, (WPARAM)W_bFIS, IPC_GET_BASIC_FILE_INFOW);
		
		ReadProcessMemory(pWinamp, W_bFIS, &bFIS, sizeof(bFIS), NULL);
		//wchar_t wbuff[256];
		//ReadProcessMemory(pWinamp, bFIS.title, &wbuff, bFIS.titlelen, NULL); not yet implemented
		// TODO implement missing fields, tile has to be sanitized (see search)

		VirtualFreeEx(pWinamp, W_bFIS, 0, MEM_DECOMMIT);

		CloseHandle(pWinamp);
		free(fileN);

		sprintf(buff, "file: %s\nTime: %d\nArtist: nA\nTitle: nA\n", mbuff, bFIS.length);

		send(clientData.clisockfd, F(buff), 0);
		
	}

	CloseHandle(pWinamp);
	return 0;
}

int pause(clientThreadData_t &clientData) {
	SendMessage(hWinamp, WM_COMMAND, WINAMP_BUTTON3, 0);
	return 0;
}

int search(clientThreadData_t &clientData) {
	if (clientData.argCount < 2) return 1;
	if (clientData.argCount % 2 != 0) return 1;
	if (clientData.args[0].argType != STRING) return 2;
	if (clientData.args[1].argType != STRING) return 2;

	char searchTerm[256];
	char searchQuery[1024];
	wchar_t searchQueryW[1024];
	int sqi = 0;

	for (int gi = 0; gi < clientData.argCount; gi += 2) {

		if (!strcmp("any", clientData.args[gi].argValue)) return 99; // Not (yet) implemented
		// TODO implement base, file, ... qualifiers
		// TODO implement sort, window qualifiers

		// TODO Sanity check with the arg length
		// TODO The above strcmp may read too far into the heap as the argValue ist not \0 terminated...

		
		memcpy(searchTerm, clientData.args[gi + 1].argValue, clientData.args[gi + 1].argLen);
		searchTerm[clientData.args[gi + 1].argLen] = '\0';
		pyUnescapeInPlace(searchTerm, clientData.args[gi + 1].argLen + 1);

		if (sqi > 0) sqi += sprintf(&searchQuery[sqi], " AND ");
		sqi += sprintf(&searchQuery[sqi], "((title has \"%s\") OR (artist has \"%s\") OR (album has \"%s\"))", searchTerm, searchTerm, searchTerm);

	}


		//printf("Searching for \"%s\"...\r\n", searchTerm);

		utf8ToUtf16(searchQuery, searchQueryW);

		clientData.scanner->Query(searchQueryW);
		for (clientData.scanner->First(); !clientData.scanner->Eof(); clientData.scanner->Next()) {
			FilenameField *fileName = (FilenameField *)clientData.scanner->GetFieldByName("filename");
			StringField *title = (StringField *)clientData.scanner->GetFieldByName("title");
			StringField *artist = (StringField *)clientData.scanner->GetFieldByName("artist");
			IntegerField *lengthF = (IntegerField *)clientData.scanner->GetFieldByName("length"); // in seconds

			char buff[2048];
			char fileNameU8[MAX_PATH*4]; // Worst case: all chars are 4 byte surrogates
			char titleU8[256];
			char artistU8[256];

			if (!fileName) continue;

			utf16ToUtf8(fileName->GetStringW(), fileNameU8);
			if (title) utf16ToUtf8(title->GetStringW(), titleU8); else sprintf(titleU8, "Unknown");
			if (artist) utf16ToUtf8(artist->GetStringW(), artistU8); else sprintf(artistU8, "Unknown");


			int length = 0;
			if (lengthF) length = lengthF->GetValue();

			sprintf(buff, "file: %s\nTime: %d\nArtist: %s\nTitle: %s\n", fileNameU8, length, artistU8, titleU8);
			send(clientData.clisockfd, F(buff), 0);
		}

		// TODO backslashes and ticks seem to break python, mask?


	return 0;
}



int add(clientThreadData_t &clientData) {
	if (clientData.argCount != 1) return 1;
	if (clientData.args[0].argType != STRING) return 2;
	char fileName[512];
	memcpy(fileName, clientData.args[0].argValue, clientData.args[0].argLen);
	fileName[clientData.args[0].argLen] = '\0';
	pyUnescapeInPlace(fileName, clientData.args[0].argLen+1);

	printf("Adding the new file \"%s\"...\r\n", fileName);

	wchar_t wbuff[512];
	utf8ToUtf16(fileName, wbuff);

	DWORD dwWinampPID;
	GetWindowThreadProcessId(hWinamp, &dwWinampPID);
	HANDLE pWinamp;
	pWinamp = OpenProcess(PROCESS_ALL_ACCESS, 0, dwWinampPID);

	// Allocate memory inside Winamp's address space to exchange data with it
	char *W_eFWMS = (char *)VirtualAllocEx(pWinamp, NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
	wchar_t *W_eFWMSFileName = (wchar_t*)(W_eFWMS + 1024);
	//wchar_t *W_eFWMSTitle = (wchar_t*)(W_eFWMS + 2048); title is NULL, so we dont need this right now

	enqueueFileWithMetaStructW eFWMS = { 0 };
	eFWMS.filename = W_eFWMSFileName;
	//eFWMS.title = W_eFWMSTitle;
	eFWMS.title = NULL;
	eFWMS.length = 300;  // this is the number of seconds for the track
	// TODO: Whats with the length, set to 0?
	WriteProcessMemory(pWinamp, W_eFWMS, &eFWMS, sizeof(eFWMS), NULL);

	// Test: L"C:\\Users\\Alexander\\Desktop\\Musik\\Gangnam Style - PSY.mp3"
	WriteProcessMemory(pWinamp, W_eFWMSFileName, wbuff, sizeof(wchar_t)*(wcslen(wbuff) + 1), NULL);
	// WriteProcessMemory(pWinamp, W_eFWMSTitle, L"", sizeof(wchar_t)*(wcslen(key) + 1), NULL); title is NULL, so we dont need this right now

	SendMessage(hWinamp, WM_WA_IPC, (WPARAM)W_eFWMS, IPC_ENQUEUEFILEW);

	VirtualFreeEx(pWinamp, W_eFWMS, 0, MEM_DECOMMIT);

	CloseHandle(pWinamp);

	return 0;
}

int play(clientThreadData_t &clientData) {
	SendMessage(hWinamp, WM_WA_IPC, 0, IPC_STARTPLAY);
	return 0;
}

int next(clientThreadData_t &clientData) {
	SendMessage(hWinamp, WM_COMMAND, WINAMP_BUTTON5, 0);
	return 0;
}

void sendResponse(clientThreadData_t &clientData, int retVal) {
	switch (retVal) {
		case 0:
			send(clientData.clisockfd, F("OK\n"), 0);
			break;
		default:
			printf("ERROR: Command returned an error, code: %d\r\n", retVal);
			send(clientData.clisockfd, F("ACK ERROR\n"), 0);
			break;
	}
}

void CT_updateData(clientThreadData_t &clientData) {
	clientData.now = GetTickCount();
}

void CT_waitSetRef(clientThreadData_t &clientData) {
	clientData.waitStamp = clientData.now;
}

void CT_waitNextState(clientThreadData_t &clientData, unsigned long delay, protocolState nextState) {
	if (clientData.now - clientData.waitStamp > delay) clientData.protoState = nextState;
}

void SS_timer(clientThreadData_t &clientData, unsigned long delay, protocolState nextState) {
	if (clientData.now - clientData.timerStamps[nextState] > delay) {
		clientData.protoState = nextState;
		clientData.timerStamps[nextState] = clientData.now;
	}
}

int tryConsumeChar(char which, protocolState nextState, char *lineBuf, int lineBufSz, int *parserOffset, protocolState *state) {
	if (*parserOffset + 1 > lineBufSz) return 0; // Sanity check!
	if (lineBuf[*parserOffset] == which) {
		*state = nextState;
		(*parserOffset)++;
		return 1;
	}
	return 0;
}

int tryConsumeString(const char *which, int whichSz, protocolState nextState, char *lineBuf, int lineBufSz, int *parserOffset, protocolState *state) {
	if (*parserOffset + 1 > lineBufSz) return 0; // Sanity check!
	if (!memcmp(&lineBuf[*parserOffset], which, whichSz)) {
		*state = nextState;
		(*parserOffset) += whichSz;
		return 1;
	}
	return 0;
}

int tryConsumeAndGetUntilChar(char which, char **whereToPut, int *howManyCharsFound, protocolState nextState, char *lineBuf, int lineBufSz, int *parserOffset, protocolState *state) {
	char *firstFound;
	firstFound = (char *)memchr(&lineBuf[*parserOffset], which, lineBufSz - *parserOffset);
	if (firstFound == NULL) return 0;
	*whereToPut = &lineBuf[*parserOffset];
	*state = nextState;
	*howManyCharsFound = firstFound - &lineBuf[*parserOffset];
	*parserOffset += *howManyCharsFound;
	return 1;
}

void makeNewArgument(argumentType argType, int argLength, char *arg, int *argCount, argument_t **args) {
	(*argCount)++;
	*args = (argument_t *)realloc(*args, (*argCount) * sizeof(**args));
	(*args)[*argCount - 1].argLen = argLength;
	(*args)[*argCount - 1].argType = argType;
	(*args)[*argCount - 1].argValue = arg;
	return;
}

int tryConsumeNewArgumentUntilChar(argumentType argType, char which, int *argCount, argument_t **args, protocolState nextState, char *lineBuf, int lineBufSz, int *parserOffset, protocolState *state) {
	char *arg = NULL;
	int argLength = 0;
	if (tryConsumeAndGetUntilChar(which, &arg, &argLength, nextState, lineBuf, lineBufSz, parserOffset, state)) {
		makeNewArgument(argType, argLength, arg, argCount, args);
		return 1;
	}
	return 0;
}

void handleProtocol(clientThreadData_t &clientData) {
	int	c;
#define tryConsumeChar(c, st) tryConsumeChar(c, st, clientData.lineBuf, clientData.lineBufSz, &clientData.parserOffset, &clientData.protoState) // To save us from typing that out all the time...
#define tryConsumeString(s, st) tryConsumeString(s, strlen(s), st, clientData.lineBuf, clientData.lineBufSz, &clientData.parserOffset, &clientData.protoState) // To save us from typing that out all the time...
#define isParserStackEmpty() (clientData.parserOffset == clientData.lineBufSz)
#define getRemainingInput() (&clientData.lineBuf[clientData.parserOffset])
#define getRemainingInputSize() (clientData.lineBufSz - clientData.parserOffset)
#define getRemainingAsNewArgument(t, pAC, ppAS) makeNewArgument(t, getRemainingInputSize(), getRemainingInput(), pAC, ppAS)
#define tryConsumeAndGetUntilChar(c, ppB, pBSz, st) tryConsumeAndGetUntilChar(c, ppB, pBSz, st, clientData.lineBuf, clientData.lineBufSz, &clientData.parserOffset, &clientData.protoState) // To save us from typing that out all the time...
#define tryConsumeNewArgumentUntilChar(t, c, pAC, ppAS, st) tryConsumeNewArgumentUntilChar(t, c, pAC, ppAS, st, clientData.lineBuf, clientData.lineBufSz, &clientData.parserOffset, &clientData.protoState) // To save us from typing that out all the time...
#define tryAcceptRequest(cD, cmd, st) isParserStackEmpty() ? cD.protoState = st, sendResponse(cD, cmd(cD)), true : false
#define tryAcceptRequestS(cD, st) isParserStackEmpty() ? cD.protoState = st, true : false
	unsigned long nRead;
	DWORD dwBytesRet = 0;
	int ret = WSAIoctl(clientData.clisockfd, FIONREAD, NULL, 0, &nRead, sizeof(nRead), &dwBytesRet, NULL, NULL);
	if (ret < 0) {
		ret = WSAGetLastError();
		switch (ret) {
			case WSAEWOULDBLOCK:
				// That's ok, nothing bad
				break;
			default:
				// Something went wrong, probably the client disconnected...
				printf("INFO: The client has disconnected at its own request.\r\n");
				shutdown(clientData.clisockfd, SD_BOTH);
				closesocket(clientData.clisockfd);
				if (clientData.args) free(clientData.args);
				clientData.clisockfd = -1;
				return;
		}
	}
	if (clientData.clisockfd >= 0) {
		switch (clientData.protoState) {
		case RESET:
			clientData.lineBufSz = 0; // Discard all
			clientData.parserOffset = 0;
			clientData.protoState = READLINE;
			clientData.argCount = 0;
			if (clientData.args) free(clientData.args);
			clientData.args = NULL;
			break;
		case READLINE:
			//if (!clientData.client.available()) return; // continue;
			char buffer[2];
			memset(buffer, 0, sizeof(buffer));
			int ret;
			ret = recv(clientData.clisockfd, buffer, 1, 0);
			if (ret == 0) {
				printf("INFO: The client has disconnected at its own request.\r\n");
				shutdown(clientData.clisockfd, SD_BOTH);
				closesocket(clientData.clisockfd);
				if (clientData.args) free(clientData.args);
				clientData.clisockfd = -1;
				return;
			} else if (ret < 0) {
				if (ret == SOCKET_ERROR) ret = WSAGetLastError();
				switch (ret) {
					case WSAEMSGSIZE:
					case 0:
						// We're good, this is not a real error
						break;
					case EAGAIN:
					case WSAEWOULDBLOCK:
						// Would block
						Sleep(100);
						return;
						break;
					default:
						printf("ERROR: reading from the client failed! Disconnecting. code: %d\r\n", ret);
						shutdown(clientData.clisockfd, SD_BOTH);
						closesocket(clientData.clisockfd);
						if (clientData.args) free(clientData.args);
						clientData.clisockfd = -1;
						return;
				}
			}

			c = *buffer;

			switch (c) {
			case -1:
				return; // continue; // No data, try again
			case '\r':
			case '\n':
				if (clientData.lineBufSz > 0) clientData.protoState = PARSE_START;
				// Discard those, they are stop delimiters
				return; // continue;
			}
			if (clientData.lineBufSz >= IN_BUFFER_SIZE - 1) {
				// Already too much, stop it!
				clientData.protoState = ERROR_BUFF_OVERFLOW;
				return; // continue;
			}
			clientData.lineBuf[clientData.lineBufSz++] = (unsigned char)(0xFF & c); // Copy to buffer
			break;
		case ERROR_BUFFER_OVERFLOW:
			// When there was an overflow, we'll call it quits!
			printf("ERROR: BUFFER OVERFLOW\r\n");
			clientData.protoState = RESET;
			break;
		case PARSE_START:
			clientData.lineBuf[clientData.lineBufSz] = '\0';
			printf("PARSER: New Request: \"%s\"\r\n", clientData.lineBuf);
			// Readline is done, start interpreting what we got
			if (tryConsumeString("clear", PARSE_CLEAR)) break;
			if (tryConsumeString("consume", PARSE_CONSUME)) break;
			if (tryConsumeString("status", PARSE_STATUS)) break;
			if (tryConsumeString("playlistinfo", PARSE_PLAYLISTINFO)) break;
			if (tryConsumeString("pause", PARSE_PAUSE)) break;
			if (tryConsumeString("search", PARSE_SEARCH)) break;
			if (tryConsumeString("add", PARSE_ADD)) break;
			if (tryConsumeString("play", PARSE_PLAY)) break;
			if (tryConsumeString("next", PARSE_NEXT)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case ERROR_MALFORMED_INPUT:
			printf("ERROR: MALFORMED INPUT at offset %d\r\n", clientData.parserOffset); // %d parserOffset
			sendResponse(clientData, -1);
			clientData.protoState = RESET;
			break;
		case PARSE_CLEAR:
			if (tryAcceptRequest(clientData, clear, RESET)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_CONSUME:
			if (tryConsumeChar(' ', PARSE_CONSUME_ARG1)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_CONSUME_ARG1:
			clientData.protoState = RESET;
			getRemainingAsNewArgument(INTEGER, &clientData.argCount, &clientData.args);
			sendResponse(clientData, consume(clientData));
			break;
		case PARSE_STATUS:
			if (tryAcceptRequest(clientData, status, RESET)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_PLAYLISTINFO:
			if (tryAcceptRequest(clientData, playlistinfo, RESET)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			// TODO: Implement optional arguments
			break;
		case PARSE_PAUSE:
			if (tryAcceptRequest(clientData, pause, RESET)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_SEARCH:
			if (tryConsumeChar(' ', PARSE_SEARCH_ARG1)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_SEARCH_ARG1_EX:
			// TODO implement optional syntax here, i.e. sort, window, ...
			// no break by intention
		case PARSE_SEARCH_ARG1:
			// {TYPE} from (any, file, base, modified-since)
			if (tryConsumeNewArgumentUntilChar(STRING, ' ', &clientData.argCount, &clientData.args, PARSE_SEARCH_DELIMITER1)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_SEARCH_DELIMITER1:
			if (tryConsumeString(" \"", PARSE_SEARCH_ARG2)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_SEARCH_ARG2:
			// {"WHAT"}
			// TODO extra " will break this
			if (tryConsumeNewArgumentUntilChar(STRING, '"', &clientData.argCount, &clientData.args, PARSE_SEARCH_DELIMITER2)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_SEARCH_DELIMITER2:
			if (tryConsumeChar('"', PARSE_SEARCH_ARG3)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_SEARCH_ARG3:
			CT_waitSetRef(clientData);
			if (tryAcceptRequestS(clientData, PARSE_SEARCH_WAIT)) break;
			if (tryConsumeChar(' ', PARSE_SEARCH_ARG1_EX)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_SEARCH_WAIT:
			CT_waitNextState(clientData, 0, PARSE_SEARCH_EXECUTE); // TODO implement the abort new search feature ... 1000 ms should do
			break;
		case PARSE_SEARCH_EXECUTE:
			sendResponse(clientData, search(clientData));
			clientData.protoState = RESET;
			break;
		case PARSE_ADD:
			if (tryConsumeString(" \"", PARSE_ADD_ARG1)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_ADD_ARG1:
			if (tryConsumeNewArgumentUntilChar(STRING, '"', &clientData.argCount, &clientData.args, PARSE_ADD_DELIMITER1)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_ADD_DELIMITER1:
			if (tryConsumeChar('"', PARSE_ADD_ARG2)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_ADD_ARG2:
			if (tryAcceptRequest(clientData, add, RESET)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		case PARSE_PLAY:
			if (tryAcceptRequest(clientData, play, RESET)) break;
			else {
				// TODO: implement additional syntax
				clientData.protoState = ERROR_MALFORMED_INPUT;
			}
			break;
		case PARSE_NEXT:
			if (tryAcceptRequest(clientData, next, RESET)) break;
			clientData.protoState = ERROR_MALFORMED_INPUT;
			break;
		default:
			break;
		}
	}
}

void clientThread(clientThreadData_t &clientData) {
	while (clientData.clisockfd >= 0 && isRunning) {
		CT_updateData(clientData);
		handleProtocol(clientData);
	}
	if (clientData.clisockfd >= 0) {
		shutdown(clientData.clisockfd, SD_BOTH);
		closesocket(clientData.clisockfd);
		if (clientData.args) free(clientData.args);
	}
	if (clientData.scanner) clientData.table->DeleteScanner(clientData.scanner);
	if (clientData.table) db.CloseTable(clientData.table);
	clientData.isTerminated = true;
}

BOOL WINAPI HandlerRoutine(_In_ DWORD dwCtrlType) {
	switch (dwCtrlType) {
		case CTRL_C_EVENT:
			printf("The application was requested to terminate by Ctrl+C...\r\n");
			isRunning = false;
			// Signal is handled - don't pass it on to the next handler
			return TRUE;
		default:
			// Pass signal on to the next handler
			return FALSE;
	}
}


int main() {
	clientThreadData_t clientThreads[MAX_CONNECTIONS];
	int numClientThreads = 0;
	int ret;
	Table *table;

	SetConsoleCtrlHandler(HandlerRoutine, TRUE);

	printf("MPDInterface for Winamp by e01f\r\n");
	printf("\r\n");

	printf("Contacting Winamp... ");
	hWinamp = NULL;
	hWinamp = FindWindow("Winamp v1.x", NULL);
	if (hWinamp <= 0) {
		printf("ERROR: could not find the running winamp instance!\r\n");
		goto exitWAStage;
	}
	printf("Success.\r\n");
	
	printf("Loading database... ");
	// open the media library's database by passing the filenames of the data file and index file
	// we have to tell the database not to create the table and index
	TCHAR szWAADPath[MAX_PATH];
	if (!SUCCEEDED(SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, szWAADPath))) {
		printf("ERROR: could not locate the AppData folder!\r\n");
		goto exitWAStage;
	}
	PathAppend(szWAADPath, "/Winamp/Plugins/ml/");
	strcpy(szWAADIdxPath, szWAADPath);
	strcpy(szWAADDatPath, szWAADPath);
	strcat(szWAADIdxPath, "main.idx");
	strcat(szWAADDatPath, "main.dat");
	table = db.OpenTable(szWAADDatPath, szWAADIdxPath, false, true);
	if (!table)  {
		printf("ERROR: could not open table files!\r\n");
		goto exitWAStage;
	}
	printf("Success. %d records in the database.\r\n", table->GetRecordsCount());

	printf("Starting to listen on port %d (AF_INET)...\r\n", PORT);
	WSADATA wsaData;
	ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (ret < 0) {
		printf("ERROR: could not WSAStartup!\r\n");
		goto exitDBStage;
	}
	SOCKET sockfd;
	sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sockfd < 0) {
		printf("ERROR: could not get a socket!\r\n");
		goto exitWSAStage;
	}
	char t = 1;
	ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &t, sizeof(char));
	if (ret < 0) {
		printf("ERROR: could not set socket options!\r\n");
		goto exitSOCKStage;
	}
	ret = setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &t, sizeof(char));
	if (ret < 0) {
		printf("ERROR: could not set socket options!\r\n");
		goto exitSOCKStage;
	}
	DWORD dwBytesRet = 0;
	struct tcp_keepalive alive;
	alive.onoff = TRUE;
	alive.keepalivetime = 1000;
	alive.keepaliveinterval = 2000;
	if (WSAIoctl(sockfd, SIO_KEEPALIVE_VALS, &alive, sizeof(alive), NULL, 0, &dwBytesRet, NULL, NULL) == SOCKET_ERROR) {
		printf("ERROR: could not set socket options! error %d\r\n", WSAGetLastError());
		goto exitSOCKStage;
	}
	sockaddr_in serv_addr, cli_addr;
	serv_addr = {};
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(PORT);
	ret = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
	if (ret < 0) {
		printf("ERROR: could not bind port %d (AF_INET)!\r\n", PORT);
		goto exitSOCKStage;
	}
	ret = listen(sockfd, 5);
	int clilen;
	clilen = sizeof(cli_addr);
	int clisockfd;
	while (isRunning) {

		// Check if there is a new client available...
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(sockfd, &readSet);
		timeval timeout;
		timeout.tv_sec = 0;  // Do not wait
		timeout.tv_usec = 0;
		if (select(sockfd, &readSet, NULL, NULL, &timeout) != 1) {
			// There is no new client
			continue;
		}

		clisockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
		if (clisockfd < 0) {
			printf("ERROR: could not accept a new client!\r\n");
			goto exitSOCKStage;
		}

		printf("INFO: New connection request... ");

		// Set socket to non blocking mode
		u_long tul = 1;
		ioctlsocket(clisockfd, FIONBIO, &tul);
	
		int selectedThread = -1;
		// Instantiate a new thread to deal with the new client...
		if (numClientThreads < MAX_CONNECTIONS) {
			// We still have free slots
			numClientThreads++;
			selectedThread = numClientThreads - 1;
			printf("Success.\r\n");
		} else {
			// All the slots are exhausted, try recycling one of them...
			for (int i = 0; i < numClientThreads; i++) {
				if (clientThreads[i].isTerminated) {
					//printf("The maximum number of clients slots is exhausted, but a slot could be recycled from a dead connection. Success.\r\n");
					printf("Success, recycling slot %d.\r\n", i);
					// Yes, we can recycle! Go ahead...
					clientThreads[i].clientThread->join();
					delete clientThreads[i].clientThread;
					clientThreads[i].clientThread = NULL;
					selectedThread = i;
					break;
				}
			}
			if (selectedThread < 0) {
				// There is no slots that could be recycled, reject the new client then...
				printf("ERROR: maximum number of connected clients exhausted! Rejecting new client.\r\n");
				shutdown(clisockfd, SD_BOTH);
				closesocket(clisockfd);
				continue;
			}
		}
		clientThreads[selectedThread].table = db.OpenTable(szWAADDatPath, szWAADIdxPath, false, true);
		clientThreads[selectedThread].scanner = clientThreads[selectedThread].table->NewScanner(0);
		if (!clientThreads[selectedThread].scanner) {
			printf("ERROR: could not get a new Database scanner!\r\n");
			goto exitDBStage;
		}

		// Salute the new client
		ret = send(clisockfd, F("OK MPD 0.12.2\n"), 0);
		if (ret < 0) {
			printf("ERROR: could not write the salute to the client!\r\n");
			goto exitCLISOCKStage;
		}

		clientThreads[selectedThread].protoState = RESET;
		clientThreads[selectedThread].args = NULL;
		clientThreads[selectedThread].clisockfd = clisockfd;

		clientThreads[selectedThread].waitStamp = 0;
		for (int i = 0; i < PS_NUM; i++) clientThreads[selectedThread].timerStamps[i] = 0;

		clientThreads[selectedThread].clientThread = new std::thread(clientThread, std::ref(clientThreads[selectedThread]));
		// TODO: implement new thread upon quit logic
	}
	for (int i = 0; i < numClientThreads; i++) {
		clientThreads[i].clientThread->join();
		delete clientThreads[i].clientThread;
		clientThreads[i].clientThread = NULL;
	}
	goto exitSOCKStage;
	
exitCLISOCKStage:
	closesocket(clisockfd);
exitSOCKStage:
	closesocket(sockfd);
exitWSAStage:
	WSACleanup();
exitDBStage:
	db.CloseTable(table);
exitWAStage:
	system("pause");
	return 0;
}