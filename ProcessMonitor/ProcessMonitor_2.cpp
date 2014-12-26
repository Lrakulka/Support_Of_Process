#include "ProcessMonitor_2.h"

BOOLEAN ProcessMonitor::start() {	
	if (!eventsMutex) {
		if (!(eventsMutex = CreateMutex( NULL, FALSE, NULL)))
			errorExit("Create Event Mutex");
		if (!(processStatusMutex = CreateMutex( NULL, FALSE, NULL)))
			errorExit("Create Process Status Mutex");	
		if (!(logFileMutex = CreateMutex( NULL, FALSE, NULL)))
			errorExit("Create Log File Mutex");	
		if (!(stopMonitorMutex = CreateMutex( NULL, FALSE, NULL)))
			errorExit("Create Stop Monitor Mutex");
		if (!(restartQuickProcessMutex = CreateMutex( NULL, FALSE, NULL)))
			errorExit("Create Restard Quick Process Mutex");
		if (!(loggerMutex = CreateMutex( NULL, FALSE, NULL)))
			errorExit("Create Logger Mutex");
		switch (WaitForSingleObject(eventsMutex, 5000)) {
			case WAIT_ABANDONED : { return FALSE;} //exit
			case WAIT_TIMEOUT: { return FALSE;}  //exit
			case WAIT_FAILED : errorExit("Wait For Single Object event Start Mutex"); 
		}
		if (processInfo.commandLine) {  			
			// Start process and gets process handle from command line
			ZeroMemory( &processInfo.startInfo, sizeof(processInfo.startInfo));
			processInfo.startInfo.cb = sizeof(processInfo.startInfo);
			ZeroMemory( &processInfo.information, sizeof(processInfo.information));
			if (!CreateProcess( processInfo.dir, processInfo.commandLine, NULL, 
				NULL, FALSE, 0, NULL, NULL, &processInfo.startInfo, 
				&processInfo.information )) {
				errorExit("Creating Process");
			}
			processInfo.handle = processInfo.information.hProcess;
			processInfo.id = processInfo.information.dwProcessId;
		}
		if (!processInfo.handle && processInfo.id > 0) {
			// Gets process handle from id
			processInfo.handle = OpenProcess( PROCESS_QUERY_INFORMATION |
				PROCESS_VM_READ | PROCESS_TERMINATE , FALSE, processInfo.id );
		}
		if (!processInfo.handle) {
			if (!ReleaseMutex(eventsMutex))
				errorExit("Release Mutex event Start");
			return FALSE;
		}
 
		if (!processInfo.commandLine) {				
			// Getting command line and directory of process if manual start was by id
			getProcessData(&processInfo.handle, &(processInfo.commandLine), 
				&(processInfo.dir));
		}
		if (!processInfo.logName)
			processInfo.logName = getProcessLogName(processInfo.dir);

		// Monitor thread start
		monitorHandle = (HANDLE)_beginthreadex(NULL, 0, &monitor, this, 0, NULL); 

		logger("Process manually started", NULL);
		if (onProcManuallyStarted)
			onProcManuallyStarted();
		if (!ReleaseMutex(eventsMutex))
			errorExit("Release Mutex event Start");
		return TRUE;
	}
	return FALSE;
}

BOOLEAN ProcessMonitor::stop() {
	if(stopSupport()) {
		if (!TerminateProcess(processInfo.handle, TERMINATE_CODE)) 
			errorExit("Terminate Process");
		else processStatus = ProcessStatus::stoped; // setProcessStatus can't be used.
													// Mutex already has been closed
		CloseHandle(processInfo.handle); 		
		processInfo.handle = NULL;

		logger("Process manually stoped", NULL);
		CloseHandle(loggerMutex);
		loggerMutex = NULL;
		if (hFileLog) {
			CloseHandle(hFileLog);
			hFileLog = NULL;
		}
		if (onProcManuallyStopped)
			onProcManuallyStopped();
		return TRUE;
	}
	return FALSE;
}

BOOLEAN ProcessMonitor::stopSupport() {
	if (eventsMutex) {
		switch (WaitForSingleObject(eventsMutex, 5000)) {
				case WAIT_ABANDONED : { return FALSE;} //exit
				case WAIT_TIMEOUT : { return FALSE;}  //exit
				case WAIT_FAILED : errorExit("Wait For Single Object event Stop Mutex"); 
			}
		if (!setStatusOfStopMonitorMutex(TRUE))
			return FALSE;
		// Wait for monitor to terminate
		switch (WaitForMultipleObjects(1, &monitorHandle, FALSE, 3000)) {
			case WAIT_OBJECT_0 + 0: break;
			case WAIT_TIMEOUT : { return FALSE;}
			case WAIT_FAILED : { errorExit("Wait For Multiple Objects Close Monitor"); 
				break;
			}
			default : errorExit("Wait For Multiple Objects Close Monitor");
		}
		CloseHandle(monitorHandle);
		CloseHandle(processStatusMutex);
		CloseHandle(logFileMutex);
		CloseHandle(logFileHandle);
		CloseHandle(processInfo.information.hThread);
		CloseHandle(restartQuickProcessMutex);
		if (!ReleaseMutex(eventsMutex))
			errorExit("Release Mutex event Stop");
		CloseHandle(eventsMutex);
		logger("Process support stopped", NULL);
		if (hFileLog) 
			CloseHandle(hFileLog);
		HANDLE hProcess = processInfo.handle;	// Remember process handle	
		zeroParameters();	// Zero values and process handle
		processInfo.handle = hProcess;	
		if (onProcSupportManuallyStopped)
			onProcSupportManuallyStopped();
		return TRUE;
	}
	return FALSE;
}

BOOLEAN ProcessMonitor::restart() {
	if (eventsMutex) {
		logger("The process start of manually restart", NULL);
		if (setProcessStatus(ProcessStatus::restarting) && stop() && start()) {
			logger("The process of manually restarted successfully", NULL);
			if (onProcManuallyRestarted)
				onProcManuallyRestarted();
			return TRUE;
		}
		else logger("The process of manually restart failed", NULL);
	}
	return FALSE;
}

BOOLEAN ProcessMonitor::quickRestart() {
	if (eventsMutex) {
		logger("The process start of manually quick restart", NULL);
		if (setProcessStatus(ProcessStatus::restarting) && stopMonitorMutex && 
				setStatusOfQuickRestartMutex(TRUE)) {
			logger("The process of manually quick restarted successfully", NULL);
			if (onProcManuallyRestartedQ)
				onProcManuallyRestartedQ();
			return TRUE;
		} else logger("The process of manually quick restart failed", NULL);
	}
	return FALSE;
}

VOID ProcessMonitor::zeroCrashCount() {
	this->processInfo.countCrash = NULL;
}

VOID ProcessMonitor::zeroParameters() {
	eventsMutex = processStatusMutex = logFileMutex = stopMonitorMutex = logFileHandle =
		monitorHandle = processInfo.handle = processInfo.information.hThread = 
		restartQuickProcessMutex = hFileLog = NULL;
	
	stopMonitor = processInfo.countCrash = restartQuickProcess = NULL;
	processInfo.id = -1;
	processStatus = ProcessStatus::unknown;
}

// Method for the thread that watching at the process
unsigned __stdcall ProcessMonitor::monitor(PVOID p) {
	ProcessMonitor* processMonitor = static_cast<ProcessMonitor*>(p);
	BOOLEAN stop;
	DWORD exitCode;
	while (!processMonitor->getStatusOfStopMonitorMutex()) {					
		// Getting status of process
		if (!GetExitCodeProcess(processMonitor->processInfo.handle, &exitCode))
			processMonitor->errorExit("Exit Code Process");
		// Process working
		if (exitCode == STILL_ACTIVE && processMonitor->processStatus != 
			ProcessStatus::working) {
			processMonitor->logger("Process working: Automatica", NULL);
			processMonitor->setProcessStatus(ProcessStatus::working);
		}
		// Process stopped
		if (exitCode == STATUS_WAIT_0 && processMonitor->processStatus != 
			ProcessStatus::stoped) {
			processMonitor->logger("Process was stopped: Automatica", NULL);
			processMonitor->setProcessStatus(ProcessStatus::stoped);
			processMonitor->processStopped();
			// Process restart
			if (!processMonitor->automaticProcessRestard(exitCode)) {
				processMonitor->logger("The process of automatic quick restart failed",
					NULL);
				processMonitor->setStatusOfStopMonitorMutex(TRUE);
			} else processMonitor->
					logger("The process of automatic quick restarted successfully", NULL);
		}
		// Process was crashed
		if (exitCode != STILL_ACTIVE && exitCode != STATUS_WAIT_0) {			
			processMonitor->logger("Process was crashed: Automatica : Error Code ", 
				exitCode);
			processMonitor->setProcessStatus(ProcessStatus::stoped);
			processMonitor->processCrash();
			processMonitor->logger("The process start of automatic quick restart", NULL);
			// Process restart
			if (!processMonitor->automaticProcessRestard(exitCode)) {
				processMonitor->logger("The process of automatic quick restart failed",
					NULL);
				processMonitor->setStatusOfStopMonitorMutex(TRUE);
			} else processMonitor->
					logger("The process of automatic quick restarted successfully", NULL);
		}			
		// Quick process restart without stopping support
		if (processMonitor->getStatusOfQuickRestartMutex())
			if (processMonitor->automaticProcessRestard(exitCode))
				processMonitor->setStatusOfQuickRestartMutex(FALSE);
			else processMonitor->setStatusOfStopMonitorMutex(TRUE);
		Sleep(100);
	}
	processMonitor->setStatusOfStopMonitorMutex(FALSE);
	CloseHandle(processMonitor->stopMonitorMutex);
	processMonitor->stopMonitorMutex = NULL;
	return 0;
}

BOOLEAN ProcessMonitor::automaticProcessRestard(DWORD exitCode) {
	switch (WaitForSingleObject(eventsMutex, 5000)) {
		case WAIT_ABANDONED : { return FALSE;} //exit
		case WAIT_TIMEOUT: { return FALSE;}  //exit
		case WAIT_FAILED : 
			errorExit("Wait For Single Object event Restart Mutex"); 
	}
	setProcessStatus(ProcessStatus::restarting);
	// Terminate Process if it works
	if (exitCode == STILL_ACTIVE && !TerminateProcess(processInfo.handle, 
		TERMINATE_CODE)) 
			errorExit("Terminate Process");
	CloseHandle(processInfo.information.hThread);
	CloseHandle(processInfo.handle);
	// Start process
	ZeroMemory( &processInfo.startInfo, sizeof(processInfo.startInfo));
	processInfo.startInfo.cb = sizeof(processInfo.startInfo);
	ZeroMemory( &processInfo.information, sizeof(processInfo.information));
	if (!CreateProcess( processInfo.dir, processInfo.commandLine, NULL, 
		NULL, FALSE, 0, NULL, NULL, &processInfo.startInfo, 
		&processInfo.information )) {
		errorExit("Creating Process");
	}
	processInfo.handle = processInfo.information.hProcess;
	processInfo.id = processInfo.information.dwProcessId;
	if (onProcRestard)
		onProcRestard();
	if (!ReleaseMutex(eventsMutex))
		errorExit("Release Mutex event Restart");			
	return TRUE;
}

VOID ProcessMonitor::processStopped() {
	//--------
	if (onProcStop)
		onProcStop();
}

ProcessMonitor::~ProcessMonitor() {
	// Process is not terminated. 
	// User must first call stop() for terminated process
	if (eventsMutex)
		stopSupport();
	if (loggerMutex)
		CloseHandle(loggerMutex);
	// delete automatically compare the value to zero
	delete []this->processInfo.commandLine;
	delete []this->processInfo.dir;
	delete []this->processInfo.logName;
}

ProcessMonitor::ProcessMonitor() {
	processInfo.commandLine = processInfo.logName = processInfo.dir = NULL;
	loggerMutex = NULL;
	zeroParameters();
	// Set standert logger writer
	logWriter = this->standartLoggerWriter;
}

ProcessMonitor::ProcessMonitor(DWORD argc, LPSTR argv[]) {
	LPSTR commandLine;
	zeroParameters();
	processInfo.logName = NULL;
	loggerMutex = NULL;
	// Gets Command line and directory of process
	this->processInfo.dir = new CHAR[strlen(argv[0]) + 1];
	strcpy(this->processInfo.dir, argv[0]);
	DWORD commandLineLength = 0;
	for (DWORD i = 0; i < argc; i++)
		commandLineLength += strlen(argv[i]);
	this->processInfo.commandLine = new CHAR[commandLineLength + argc];
	strcpy(this->processInfo.commandLine, argv[0]);
	commandLineLength = strlen(argv[0]);
	for (DWORD i = 1; i < argc; i++) {
		*(this->processInfo.commandLine + commandLineLength) = ' ';
		commandLineLength++;
		strcpy(this->processInfo.commandLine + commandLineLength, argv[i]);
		commandLineLength += strlen(argv[i]);
	}
	// Set standert logger writer
	logWriter = this->standartLoggerWriter;
}

ProcessMonitor::ProcessMonitor(DWORD processID) {
	processInfo.commandLine = processInfo.logName = processInfo.dir = NULL;
	loggerMutex = NULL;
	zeroParameters();
	this->processInfo.id = processID;
	// Set standert logger writer
	logWriter = this->standartLoggerWriter;
}

BOOLEAN ProcessMonitor::getProcessData(PHANDLE inHProcess, 
									LPSTR *outCommandLine, LPSTR *outDir) {
	 // determine if 64 or 32-bit processor
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);

    // determine if this process is running on WOW64
    BOOL wow;
    IsWow64Process(GetCurrentProcess(), &wow);

    // use WinDbg "dt ntdll!_PEB" command and search for ProcessParameters 
	// offset to find the truth out
    DWORD ProcessParametersOffset = 
		si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 0x20 : 0x10;
    DWORD CommandLineOffset = 
		si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 0x70 : 0x40;
	DWORD ProcessDir = 
		si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 0x60 : 0x38;

    // read basic info to get ProcessParameters address, we only need 
	// the beginning of PEB
    DWORD pebSize = ProcessParametersOffset + 8;
    PBYTE peb = new BYTE[pebSize];
    ZeroMemory(peb, pebSize);

    // read basic info to get CommandLine address, we only need the 
	// beginning of ProcessParameters
    DWORD ppSize = CommandLineOffset + 16;
    PBYTE pp = new BYTE[ppSize];
    ZeroMemory(pp, ppSize);

    PWSTR cmdLine, dir;

    if (wow)
    {
        // we're running as a 32-bit process in a 64-bit OS
        mPROCESS_BASIC_INFORMATION_WOW64 pbi;
        ZeroMemory(&pbi, sizeof(pbi));

        // get process information from 64-bit world
        _NtQueryInformationProcess query = (_NtQueryInformationProcess)
			GetProcAddress(GetModuleHandleA("ntdll.dll"), 
			"NtWow64QueryInformationProcess64");
        if (query(*inHProcess, 0, &pbi, sizeof(pbi), NULL))
        {
			errorExit("NtWow64QueryInformationProcess64 failed\n");
        }

        // read PEB from 64-bit address space
        _NtWow64ReadVirtualMemory64 read = (_NtWow64ReadVirtualMemory64)
			GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtWow64ReadVirtualMemory64");
        if (read(*inHProcess, pbi.PebBaseAddress, peb, pebSize, NULL))
        {
            errorExit("NtWow64ReadVirtualMemory64 PEB failed\n");
        }

        // read ProcessParameters from 64-bit address space
        PBYTE* parameters = (PBYTE*)*(LPVOID*)(peb + ProcessParametersOffset); // address in remote process adress space
        if (read(*inHProcess, parameters, pp, ppSize, NULL))
        {
			errorExit("NtWow64ReadVirtualMemory64 Parameters failed\n");
        }

        // read CommandLine
        mUNICODE_STRING_WOW64* pCommandLine = (mUNICODE_STRING_WOW64*)(pp + CommandLineOffset);
        cmdLine = new WCHAR[pCommandLine->MaximumLength];
        if (read(*inHProcess, pCommandLine->Buffer, cmdLine, pCommandLine->MaximumLength, NULL))
        {
			errorExit("NtWow64ReadVirtualMemory64 Parameters failed\n");
        }
		// read process dir
		mUNICODE_STRING_WOW64* pDir = (mUNICODE_STRING_WOW64*)(pp + ProcessDir);
        dir = new WCHAR[pDir->MaximumLength];
        if (read(*inHProcess, pDir->Buffer, dir, pDir->MaximumLength, NULL))
        {
			errorExit("NtWow64ReadVirtualMemory64 Parameters failed\n");
        }
		// Convert from WCHAR to CHAR. Wchar uses 2byte and CHAR 1byte
		*outCommandLine = new CHAR[pCommandLine->Length];
		size_t len = wcstombs(*outCommandLine, cmdLine, 
			wcslen(cmdLine));
	    if(pCommandLine->Length > 0) 
			 (*outCommandLine)[pCommandLine->Length / 2] = '\0';
	    *outDir = new CHAR[pDir->Length];
	    len = wcstombs(*outDir, dir, 
			wcslen(dir));
	    if(pDir->Length > 0) 
			(*outDir)[pDir->Length / 2] = '\0';
    }
    else
    {
        // we're running as a 32-bit process in a 32-bit OS, or 
		// as a 64-bit process in a 64-bit OS
        mPROCESS_BASIC_INFORMATION pbi;
        ZeroMemory(&pbi, sizeof(pbi));

        // get process information
        _NtQueryInformationProcess query = (_NtQueryInformationProcess)
			GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
        if (query(*inHProcess, 0, &pbi, sizeof(pbi), NULL))
        {
			errorExit("NtQueryInformationProcess failed\n");
        }

        // read PEB
		if (!ReadProcessMemory(*inHProcess, pbi.PebBaseAddress, peb, pebSize, NULL))
        {
			errorExit("ReadProcessMemory PEB failed\n");
        }

        // read ProcessParameters
		// address in remote process adress space
        PBYTE* parameters = (PBYTE*)*(LPVOID*)(peb + ProcessParametersOffset);
		if (!ReadProcessMemory(*inHProcess, parameters, pp, ppSize, NULL))
        {
            printf("ReadProcessMemory Parameters failed\n");
            CloseHandle(*inHProcess);
            return -1;
        }

        // read CommandLine
        mUNICODE_STRING* pCommandLine = (mUNICODE_STRING*)(pp + CommandLineOffset);
        cmdLine = new WCHAR[pCommandLine->MaximumLength];
        if (!ReadProcessMemory(*inHProcess, pCommandLine->Buffer, cmdLine, 
			pCommandLine->MaximumLength, NULL))
        {
			errorExit("ReadProcessMemory Parameters failed\n");
        }
		// read process dir
		mUNICODE_STRING* pDir = (mUNICODE_STRING*)(pp + ProcessDir);
        dir = new WCHAR[pDir->MaximumLength];
        if (!ReadProcessMemory(*inHProcess, pDir->Buffer, dir, pDir->MaximumLength, NULL))
        {
			errorExit("ReadProcessMemory Parameters failed\n");
        }
		// Convert from WCHAR to CHAR. Wchar uses 2byte and CHAR 1byte
		*outCommandLine = new CHAR[pCommandLine->Length];
		size_t len = wcstombs(*outCommandLine, cmdLine, 
			wcslen(cmdLine));
	    if(pCommandLine->Length > 0) 
			 (*outCommandLine)[pCommandLine->Length / 2] = '\0';
	    *outDir = new CHAR[pDir->Length];
	    len = wcstombs(*outDir, dir, 
			wcslen(dir));
	    if(pDir->Length > 0) 
			(*outDir)[pDir->Length / 2] = '\0';
    }
	delete []peb;
	delete []pp;
	delete []cmdLine;
	delete []dir;
}

LPSTR ProcessMonitor::getProcessLogName(LPCSTR processDir) {
	DWORD beg = 0, kol = 0;
	// Getting process name + ".txt"
	if (strrchr(processDir,'\\')) {
		beg = strlen(processDir) - strlen(strrchr(processDir,'\\') + 1);
		kol = strlen(processDir) - 
					strlen(strchr(strrchr(processDir,'\\'), '.')) - beg;
	} else	kol = strlen(processDir) - 
					strlen(strchr(processDir, '.')) - beg;
	LPSTR processName = new CHAR[kol + 5];
	strncpy(processName, processDir + beg, kol);
	processName[kol] = '\0';
	strcat(processName, ".txt");
	return processName;
}

VOID ProcessMonitor::errorExit(LPSTR lpszFunction) 
{ 
    // Retrieve the system error message for the last-error code

    LPVOID lpMsgBuf;
    LPVOID lpDisplayBuf;
    DWORD dw = GetLastError(); 

    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        dw,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &lpMsgBuf,
        0, NULL );

    // Display the error message and exit the process

    lpDisplayBuf = (LPVOID)LocalAlloc(LMEM_ZEROINIT, 
        (lstrlen((LPCTSTR)lpMsgBuf) + lstrlen((LPCTSTR)lpszFunction) + 
		40) * sizeof(TCHAR)); 
	StringCchPrintf((LPTSTR)lpDisplayBuf, 
        LocalSize(lpDisplayBuf) / sizeof(TCHAR),
        TEXT("%s failed with error %d: %s"), 
        lpszFunction, dw, lpMsgBuf); 
    MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK); 

    LocalFree(lpMsgBuf);
    LocalFree(lpDisplayBuf);
    ExitProcess(dw);
}

BOOLEAN ProcessMonitor::getStatusOfStopMonitorMutex() {	
	BOOLEAN result;
	switch (WaitForSingleObject(stopMonitorMutex, 5000)) {
		case WAIT_ABANDONED : { result = TRUE; break;} //exit
		case WAIT_OBJECT_0 : { result = stopMonitor; break;}
		case WAIT_TIMEOUT: { result = TRUE;}  //exit
		case WAIT_FAILED : 
			errorExit("WaitForSingleObject Get Status Of Stop Monitor Mutex"); 
	}
	if (!ReleaseMutex(stopMonitorMutex))
		errorExit("Release Get Status Of Stop Monitor Mutex");
	return result;
}

BOOLEAN ProcessMonitor::getStatusOfQuickRestartMutex() {	
	BOOLEAN result;
	switch (WaitForSingleObject(restartQuickProcessMutex, 5000)) {
		case WAIT_ABANDONED : { result = TRUE; break;} //exit
		case WAIT_OBJECT_0 : { result = restartQuickProcess; break;}
		case WAIT_TIMEOUT: { result = TRUE;}  //exit
		case WAIT_FAILED : 
			errorExit("WaitForSingleObject Get Status Of Quick Restart Process Mutex"); 
	}
	if (!ReleaseMutex(restartQuickProcessMutex))
		errorExit("Release Get Status Of Quick Restart Process Mutex");
	return result;
}

// unknown=0 working=1 stoped=2 restarting=3 
CHAR ProcessMonitor::getProcessStatus() {	
	CHAR result = 0;
	if (processStatusMutex) {
		switch (WaitForSingleObject(processStatusMutex, 5000)) {
			case WAIT_ABANDONED : { result = -1; break;}
			case WAIT_OBJECT_0 : { result = (CHAR)processStatus; break;}
			case WAIT_TIMEOUT: { result = -2;}
			case WAIT_FAILED : errorExit("WaitForSingleObject Get Process Status Mutex"); 
		}
		if (!ReleaseMutex(processStatusMutex))
			errorExit("ReleaseMutex Get Process Status Mutex");
	}
	return result;
}

HANDLE ProcessMonitor::getProcHandle() {
	if (!eventsMutex)
		return NULL;
	return processInfo.handle;
}

BOOLEAN ProcessMonitor::setStatusOfStopMonitorMutex(BOOLEAN status) {
	switch (WaitForSingleObject(stopMonitorMutex, 5000)) {
		case WAIT_ABANDONED : { return FALSE;}
		case WAIT_OBJECT_0 : { this->stopMonitor = status; break;}
		case WAIT_TIMEOUT: { return FALSE;}
		case WAIT_FAILED : errorExit("WaitForSingleObject Set Status Of Stop Monitor"); 
	}
	if (!ReleaseMutex(stopMonitorMutex))
		errorExit("Release Set Status Of Stop Monitor Mutex");
	return TRUE;
}

BOOLEAN ProcessMonitor::setStatusOfQuickRestartMutex(BOOLEAN status) {
	switch (WaitForSingleObject(restartQuickProcessMutex, 5000)) {
		case WAIT_ABANDONED : { return FALSE;}
		case WAIT_OBJECT_0 : { this->restartQuickProcess = status; break;}
		case WAIT_TIMEOUT: { return FALSE;}
		case WAIT_FAILED : errorExit("WaitForSingleObject Set Status Of Quick Restart Process"); 
	}
	if (!ReleaseMutex(restartQuickProcessMutex))
		errorExit("Release Set Status Of Quick Restart Process");
	return TRUE;
}

BOOLEAN ProcessMonitor::setProcessStatus(ProcessStatus status) {
	switch (WaitForSingleObject(processStatusMutex, 5000)) {
		case WAIT_ABANDONED : { return FALSE;}
		case WAIT_OBJECT_0 : { this->processStatus = status; break;}
		case WAIT_TIMEOUT: { return FALSE;}
		case WAIT_FAILED : errorExit("WaitForSingleObject Set Process Status Mutex"); 
	}
	if (!ReleaseMutex(processStatusMutex))
		errorExit("Release Set Process Status Mutex");
	return TRUE;
}

VOID ProcessMonitor::processCrash() {	
	this->processInfo.countCrash++;
	// if process crashed more times then value of MAX_SIZE_OF_CRASH then stop
	if (this->processInfo.countCrash >= MAX_SIZE_OF_CRASH) {
		logger("Reached a critical number of crashes: Crash Count ", 
			this->processInfo.countCrash);
		errorExit("Too much crashes");
	}	
	if (onProcCrash)
		onProcCrash();
}

VOID ProcessMonitor::setProcCommandLine(DWORD argc, LPSTR argv[]) {
	delete []processInfo.commandLine;
	delete []processInfo.dir;
	delete []processInfo.logName;
	// Sets command line and directory of process
	LPSTR commandLine;
	zeroParameters();
	processInfo.logName = NULL;
	this->processInfo.dir = new CHAR[strlen(argv[0]) + 1];
	strcpy(this->processInfo.dir, argv[0]);
	DWORD commandLineLength = 0;
	for (DWORD i = 0; i < argc; i++)
		commandLineLength += strlen(argv[i]);
	this->processInfo.commandLine = new CHAR[commandLineLength + argc];
	strcpy(this->processInfo.commandLine, argv[0]);
	commandLineLength = strlen(argv[0]);
	for (DWORD i = 1; i < argc; i++) {
		*(this->processInfo.commandLine + commandLineLength) = ' ';
		commandLineLength++;
		strcpy(this->processInfo.commandLine + commandLineLength, argv[i]);
		commandLineLength += strlen(argv[i]);
	}
}

VOID ProcessMonitor::setDirOfLogWriter(LPCSTR loggerDir) {
	delete []processInfo.logName;
	processInfo.logName = new CHAR[strlen(loggerDir) + 1];
	strcpy(processInfo.logName, loggerDir);
}

VOID ProcessMonitor::setProcID(DWORD processID) {
	delete []processInfo.commandLine;
	delete []processInfo.dir;
	delete []processInfo.logName;
	processInfo.logName = processInfo.commandLine = processInfo.dir = NULL;
	processInfo.id = processID;
}

VOID ProcessMonitor::setLogWriter(LoggerMethod logWriter) {
	if (hFileLog) {
		CloseHandle(hFileLog);
		hFileLog = NULL;
	}
	this->logWriter = logWriter;
}

VOID ProcessMonitor::setOnProcStop(CallBackT onProcStop) {
	this->onProcStop = onProcStop;
}

VOID ProcessMonitor::setOnProcRestard(CallBackT onProcRestard) {
	this->onProcRestard = onProcRestard;
}

VOID ProcessMonitor::setOnProcCrash(CallBackT onProcCrash) {
	this->onProcCrash = onProcCrash;
}

VOID ProcessMonitor::setOnProcManuallyStarted(CallBackT onProcManuallyStarted) {
	this->onProcManuallyStarted = onProcManuallyStarted;
}

VOID ProcessMonitor::setOnProcManuallyStopped(CallBackT onProcManuallyStopped) {
	this->onProcManuallyStopped = onProcManuallyStopped;
}

VOID ProcessMonitor::setOnProcManuallyRestarted(CallBackT onProcManuallyRestarted) {
	this->onProcManuallyRestarted = onProcManuallyRestarted;
}

VOID ProcessMonitor::setOnProcManuallyRestartedQ(CallBackT onProcManuallyRestartedQ) {
	this->onProcManuallyRestartedQ = onProcManuallyRestartedQ;
}

VOID ProcessMonitor::setOnProcManuallySupportStopped(CallBackT 
													 onProcSupportManuallyStopped) {
	this->onProcSupportManuallyStopped = onProcSupportManuallyStopped;
}

DWORD ProcessMonitor::getCountCrash() {
	return processInfo.countCrash;
}

DWORD ProcessMonitor::getProcID() {
	return  processInfo.id;
}

LPCSTR ProcessMonitor::getProcCommandLine() {
	return  processInfo.commandLine;
}

LPCSTR ProcessMonitor::getProcDir() {
	return  processInfo.dir;
}

LPCSTR ProcessMonitor::getDirOfLogWriter() {
	return processInfo.logName;
}

LoggerMethod ProcessMonitor::getlogWriter() {
	return this->logWriter;
}

CallBackT ProcessMonitor::getOnProcStop() {
	return this->onProcStop;
}

CallBackT ProcessMonitor::getOnProcCrash() {
	return this->onProcCrash;
}

CallBackT ProcessMonitor::getOnProcRestard() {
	return this->onProcRestard;
}

CallBackT ProcessMonitor::getOnProcManuallyStarted() {
	return this->onProcManuallyStarted;
}

CallBackT ProcessMonitor::getOnProcManuallyStopped() {
	return this->onProcManuallyStopped;
}

CallBackT ProcessMonitor::getOnProcManuallyRestarted() {
	return this->onProcManuallyRestarted;
}

CallBackT ProcessMonitor::getOnProcManuallyRestartedQ() {
	return this->onProcManuallyRestartedQ;
}

CallBackT ProcessMonitor::setOnProcManuallySupportStopped() {
	return this->onProcSupportManuallyStopped;
}

VOID ProcessMonitor::logger(PCHAR message, DWORD code) {
	//	Wait until previous work with logger will be finished
	switch (WaitForSingleObject(loggerMutex, 5000)) { 
		case WAIT_ABANDONED : { return;}
		case WAIT_TIMEOUT: { return;}
		case WAIT_FAILED : errorExit("WaitForSingleObject Logger Mutex"); 
	}
	logWriter(message, code, &hFileLog, this->processInfo, this->errorExit);
	if (!ReleaseMutex(loggerMutex))
		errorExit("Release Logger Mutex");
}

VOID ProcessMonitor::standartLoggerWriter(PCHAR message, DWORD code, PHANDLE hFile,
						ProcessInfo processInfo, std::function<VOID(LPSTR)> errorExit) {
	std::string buffer;
	CHAR processIdS[15] = "";
	DWORD dwBytesWritten;
	if (!*hFile) {
		// File creating or opening
				
		*hFile = CreateFile(processInfo.logName, FILE_APPEND_DATA, 
			FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); 
		if (*hFile == INVALID_HANDLE_VALUE)
			return;
		// Head of log
		buffer = "\n";
		if (processInfo.dir) {
			buffer += "\nProcess directory ";
			buffer += processInfo.dir;
		}
		buffer += std::string("\nProcess command line ") + 
			std::string(processInfo.commandLine) + std::string( "\nProcess id ") +
			std::string(itoa(processInfo.id, processIdS, 10));
		WriteFile(*hFile, buffer.c_str(), buffer.length(), &dwBytesWritten, NULL);
	}
	buffer = "\n";
	buffer += message;
	SYSTEMTIME time;
	GetLocalTime(&time);
	// Writing events in log
	if (code) 
		buffer += itoa(code, processIdS, 10);
	buffer += std::string(" Time: ") + std::string(itoa(time.wDay, processIdS, 10)) + 
		':' + std::string(itoa(time.wMonth, processIdS, 10)) + ':'+ 
		std::string(itoa(time.wYear, processIdS, 10)) + std::string(": ") +
		std::string(itoa(time.wHour, processIdS, 10)) + ':' +
		std::string(itoa(time.wMinute, processIdS, 10)) + ':' + 
		std::string(itoa(time.wSecond, processIdS, 10)) + ':' + 
		std::string(itoa(time.wMilliseconds, processIdS, 10));
	WriteFile(*hFile, buffer.c_str(), buffer.length(), &dwBytesWritten, NULL);
}