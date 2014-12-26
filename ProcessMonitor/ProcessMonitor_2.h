#include <Windows.h>
#include <Winternl.h>
#include <functional>
#include <strsafe.h>
#include <process.h>
#include <string>

#define CallBackT std::function<VOID()>
#define LoggerMethod std::function<VOID(PCHAR, DWORD, PHANDLE, ProcessMonitor::ProcessInfo, std::function<VOID(LPSTR)>)>

static const INT MAX_SIZE_OF_CRASH = 60; //The maximum number of crashes to close process
static const INT TERMINATE_CODE = 0; // Code which returns process after manual closing

// NtQueryInformationProcess for pure 32 and 64-bit processes
typedef NTSTATUS (NTAPI *_NtQueryInformationProcess)(
    IN HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    OUT PVOID ProcessInformation,
    IN ULONG ProcessInformationLength,
    OUT PULONG ReturnLength OPTIONAL
    );

typedef NTSTATUS (NTAPI *_NtReadVirtualMemory)(
    IN HANDLE ProcessHandle,
    IN PVOID BaseAddress,
    OUT PVOID Buffer,
    IN SIZE_T Size,
    OUT PSIZE_T NumberOfBytesRead);

// NtQueryInformationProcess for 32-bit process on WOW64
typedef NTSTATUS (NTAPI *_NtWow64ReadVirtualMemory64)(
    IN HANDLE ProcessHandle,
    IN PVOID64 BaseAddress,
    OUT PVOID Buffer,
    IN ULONG64 Size,
    OUT PULONG64 NumberOfBytesRead);

// PROCESS_BASIC_INFORMATION for pure 32 and 64-bit processes
typedef struct _mPROCESS_BASIC_INFORMATION {
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID Reserved3;
} mPROCESS_BASIC_INFORMATION;

// PROCESS_BASIC_INFORMATION for 32-bit process on WOW64
// The definition is quite funky, as we just lazily doubled sizes to match offsets...
typedef struct _mPROCESS_BASIC_INFORMATION_WOW64 {
    PVOID Reserved1[2];
    PVOID64 PebBaseAddress;
    PVOID Reserved2[4];
    ULONG_PTR UniqueProcessId[2];
    PVOID Reserved3[2];
} mPROCESS_BASIC_INFORMATION_WOW64;

typedef struct _mUNICODE_STRING {
  USHORT Length;
  USHORT MaximumLength;
  PWSTR  Buffer;
} mUNICODE_STRING;

typedef struct _mUNICODE_STRING_WOW64 {
  USHORT Length;
  USHORT MaximumLength;
  PVOID64 Buffer;
} mUNICODE_STRING_WOW64;

class ProcessMonitor {
	struct ProcessInfo{
		LPSTR commandLine;
		LPSTR dir;
		LPSTR logName;
		DWORD id;
		DWORD countCrash;
		HANDLE handle;
		PROCESS_INFORMATION information;
		STARTUPINFO startInfo;
	} processInfo;
	BOOLEAN stopMonitor, restartQuickProcess;
	// procStatus updated often that is why I did not put it in to the structure
	enum class ProcessStatus {unknown, working, stoped, restarting} processStatus;
	HANDLE eventsMutex, processStatusMutex, logFileMutex, stopMonitorMutex, 
		 restartQuickProcessMutex, logFileHandle, monitorHandle, hFileLog, loggerMutex;
	
	// Events callbacks
	CallBackT onProcCrash, onProcStop, onProcRestard, onProcSupportManuallyStopped,
		onProcManuallyStarted, onProcManuallyRestarted, onProcManuallyStopped, 
		onProcManuallyRestartedQ;

	//Pointer to the method create and write log of process events
	LoggerMethod logWriter; 

	// Return TRUE if monitor thread need to stop
	BOOLEAN getStatusOfStopMonitorMutex();	
	// Return TRUE if process need to restart
	BOOLEAN getStatusOfQuickRestartMutex();
	// Return process log name (process name + ".txt")			
	LPSTR getProcessLogName(LPCSTR);	
	// Gets command line and directory of process
	BOOLEAN getProcessData(PHANDLE, LPSTR *, LPSTR *);
	// Set True if you want to stop monitor
	BOOLEAN setStatusOfStopMonitorMutex(BOOLEAN);
	// Set True if you want to restart process without stoping support
	BOOLEAN setStatusOfQuickRestartMutex(BOOLEAN);
	// Method set current status of process working
	BOOLEAN setProcessStatus(ProcessStatus);			

	// Method create error Message when something go wrong
	static VOID errorExit(LPSTR);
	// Method restarts process without stopping support monitor
	BOOLEAN automaticProcessRestard(DWORD);
	// Calls when process crashed
	VOID processCrash();
	// Calls when process stopped
	VOID processStopped();		
	// Method updates most part of the variables to zero
	VOID zeroParameters();				
	
	// Method makes log of working using logWriter(pointer to the method)
	VOID logger(PCHAR, DWORD); 
	// Standart method create and write log of process events in file
	static VOID standartLoggerWriter(PCHAR, DWORD, PHANDLE, ProcessInfo, 
		std::function<VOID(LPSTR)>);

	static unsigned __stdcall monitor(PVOID);  // Body for monitor thread
public:
	ProcessMonitor(DWORD, LPSTR[]); 
	ProcessMonitor(DWORD);
	ProcessMonitor();
	~ProcessMonitor();

	BOOLEAN start();	// For manualy start
	BOOLEAN restart();  // For manualy restart
	BOOLEAN quickRestart(); // Do not stop monitor, just restard process
	BOOLEAN stop();		// Stop process and monitor
	BOOLEAN stopSupport(); // Stop monitor of the process

	// Getters and setters
	VOID setProcID(DWORD);
	VOID setProcCommandLine(DWORD, LPSTR[]);
	VOID setDirOfLogWriter(LPCSTR);
	// You can use this method to set your own logger writer. 
	// logger will be used your method for writing log.
	VOID setLogWriter(LoggerMethod);
	VOID setOnProcStop(CallBackT);
	VOID setOnProcRestard(CallBackT);
	VOID setOnProcCrash(CallBackT);
	VOID setOnProcManuallyStarted(CallBackT);
	VOID setOnProcManuallyStopped(CallBackT);
	VOID setOnProcManuallyRestarted(CallBackT);
	VOID setOnProcManuallyRestartedQ(CallBackT); // Calls when calls quickRestart
	VOID setOnProcManuallySupportStopped(CallBackT);
	HANDLE getProcHandle();
	DWORD getProcID();
	CHAR getProcessStatus();
	LPCSTR getProcCommandLine();
	LPCSTR getProcDir();
	LPCSTR getDirOfLogWriter();
	LoggerMethod getlogWriter();
	CallBackT getOnProcStop();	
	CallBackT getOnProcRestard();
	CallBackT getOnProcCrash();
	CallBackT getOnProcManuallyStarted();
	CallBackT getOnProcManuallyStopped();
	CallBackT getOnProcManuallyRestarted();
	CallBackT getOnProcManuallyRestartedQ();
	CallBackT setOnProcManuallySupportStopped();
	DWORD getCountCrash();

	VOID zeroCrashCount(); // Method updates the crash count variables to zero
};