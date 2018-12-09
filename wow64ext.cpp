#include <Windows.h>
#include <cstddef>
#include "wow64ext.h"

BOOL g_isWow64 = TRUE;

class CMemPtr
{
private:
	void** m_ptr;
	bool watchActive;

public:
	CMemPtr(void** ptr) : m_ptr(ptr), watchActive(true) {}

	~CMemPtr()
	{
		if (*m_ptr && watchActive) {
			free(*m_ptr);
			*m_ptr = 0;
		}
	}

	void disableWatch() { watchActive = false; }
};

#define WATCH(ptr) \
    CMemPtr watch_##ptr((void**)&ptr)

#define DISABLE_WATCH(ptr) \
    watch_##ptr.disableWatch()

#pragma warning(push)
#pragma warning(disable : 4409)

//注意：X64Call会清零传入参数附近的栈空间，故传入指针参数时注意要创建本地缓冲区
//具体可以参考我是如何在 NtQueryInformationProcess64 中调用的
DWORD64 __cdecl X64Call(DWORD64 func, int argC, ...)
{
	if (!g_isWow64)
		return 0;

    va_list args;
    va_start(args, argC);
    reg64 _rcx = { (argC > 0) ? argC--, va_arg(args, DWORD64) : 0 };
    reg64 _rdx = { (argC > 0) ? argC--, va_arg(args, DWORD64) : 0 };
    reg64 _r8 = { (argC > 0) ? argC--, va_arg(args, DWORD64) : 0 };
    reg64 _r9 = { (argC > 0) ? argC--, va_arg(args, DWORD64) : 0 };
    reg64 _rax = { 0 };

    reg64 restArgs = { (DWORD64)&va_arg(args, DWORD64) };
    
    // conversion to QWORD for easier use in inline assembly
    reg64 _argC = { (DWORD64)argC };
    DWORD back_esp = 0;
	WORD back_fs = 0;

    __asm
    {
        ;// reset FS segment, to properly handle RFG
        mov    back_fs, fs
        mov    eax, 0x2B
        mov    fs, ax

        ;// keep original esp in back_esp variable
        mov    back_esp, esp
        
        ;// align esp to 0x10, without aligned stack some syscalls may return errors !
        ;// (actually, for syscalls it is sufficient to align to 8, but SSE opcodes 
        ;// requires 0x10 alignment), it will be further adjusted according to the
        ;// number of arguments above 4
        and    esp, 0xFFFFFFF0

        X64_Start();

        ;// below code is compiled as x86 inline asm, but it is executed as x64 code
        ;// that's why it need sometimes REX_W() macro, right column contains detailed
        ;// transcription how it will be interpreted by CPU

        ;// fill first four arguments
  REX_W mov    ecx, _rcx.dw[0]                          ;// mov     rcx, qword ptr [_rcx]
  REX_W mov    edx, _rdx.dw[0]                          ;// mov     rdx, qword ptr [_rdx]
        push   _r8.v                                    ;// push    qword ptr [_r8]
        X64_Pop(_R8);                                   ;// pop     r8
        push   _r9.v                                    ;// push    qword ptr [_r9]
        X64_Pop(_R9);                                   ;// pop     r9
                                                        ;//
  REX_W mov    eax, _argC.dw[0]                         ;// mov     rax, qword ptr [_argC]
                                                        ;// 
        ;// final stack adjustment, according to the    ;//
        ;// number of arguments above 4                 ;// 
        test   al, 1                                    ;// test    al, 1
        jnz    _no_adjust                               ;// jnz     _no_adjust
        sub    esp, 8                                   ;// sub     rsp, 8
_no_adjust:                                             ;//
                                                        ;// 
        push   edi                                      ;// push    rdi
  REX_W mov    edi, restArgs.dw[0]                      ;// mov     rdi, qword ptr [restArgs]
                                                        ;// 
        ;// put rest of arguments on the stack          ;// 
  REX_W test   eax, eax                                 ;// test    rax, rax
        jz     _ls_e                                    ;// je      _ls_e
  REX_W lea    edi, dword ptr [edi + 8*eax - 8]         ;// lea     rdi, [rdi + rax*8 - 8]
                                                        ;// 
_ls:                                                    ;// 
  REX_W test   eax, eax                                 ;// test    rax, rax
        jz     _ls_e                                    ;// je      _ls_e
        push   dword ptr [edi]                          ;// push    qword ptr [rdi]
  REX_W sub    edi, 8                                   ;// sub     rdi, 8
  REX_W sub    eax, 1                                   ;// sub     rax, 1
        jmp    _ls                                      ;// jmp     _ls
_ls_e:                                                  ;// 
                                                        ;// 
        ;// create stack space for spilling registers   ;// 
  REX_W sub    esp, 0x20                                ;// sub     rsp, 20h
                                                        ;// 
        call   func                                     ;// call    qword ptr [func]
                                                        ;// 
        ;// cleanup stack                               ;// 
  REX_W mov    ecx, _argC.dw[0]                         ;// mov     rcx, qword ptr [_argC]
  REX_W lea    esp, dword ptr [esp + 8*ecx + 0x20]      ;// lea     rsp, [rsp + rcx*8 + 20h]
                                                        ;// 
        pop    edi                                      ;// pop     rdi
                                                        ;// 
        // set return value                             ;// 
  REX_W mov    _rax.dw[0], eax                          ;// mov     qword ptr [_rax], rax

        X64_End();

        mov    ax, ds
        mov    ss, ax
        mov    esp, back_esp

        ;// restore FS segment
        mov    ax, back_fs
        mov    fs, ax
    }
    return _rax.v;
}
#pragma warning(pop)

void getMem64(void* dstMem, DWORD64 srcMem, size_t sz)
{
    if ((nullptr == dstMem) || (0 == srcMem) || (0 == sz))
        return;

    reg64 _src = { srcMem };

    __asm
    {
        X64_Start();

        ;// below code is compiled as x86 inline asm, but it is executed as x64 code
        ;// that's why it need sometimes REX_W() macro, right column contains detailed
        ;// transcription how it will be interpreted by CPU

        push   edi                  ;// push     rdi
        push   esi                  ;// push     rsi
                                    ;//
        mov    edi, dstMem          ;// mov      edi, dword ptr [dstMem]        ; high part of RDI is zeroed
  REX_W mov    esi, _src.dw[0]      ;// mov      rsi, qword ptr [_src]
        mov    ecx, sz              ;// mov      ecx, dword ptr [sz]            ; high part of RCX is zeroed
                                    ;//
        mov    eax, ecx             ;// mov      eax, ecx
        and    eax, 3               ;// and      eax, 3
        shr    ecx, 2               ;// shr      ecx, 2
                                    ;//
        rep    movsd                ;// rep movs dword ptr [rdi], dword ptr [rsi]
                                    ;//
        test   eax, eax             ;// test     eax, eax
        je     _move_0              ;// je       _move_0
        cmp    eax, 1               ;// cmp      eax, 1
        je     _move_1              ;// je       _move_1
                                    ;//
        movsw                       ;// movs     word ptr [rdi], word ptr [rsi]
        cmp    eax, 2               ;// cmp      eax, 2
        je     _move_0              ;// je       _move_0
                                    ;//
_move_1:                            ;//
        movsb                       ;// movs     byte ptr [rdi], byte ptr [rsi]
                                    ;//
_move_0:                            ;//
        pop    esi                  ;// pop      rsi
        pop    edi                  ;// pop      rdi

        X64_End();
    }
}

bool cmpMem64(void* dstMem, DWORD64 srcMem, size_t sz)
{
    if ((nullptr == dstMem) || (0 == srcMem) || (0 == sz))
        return false;

    bool result = false;
    reg64 _src = { srcMem };
    __asm
    {
        X64_Start();

        ;// below code is compiled as x86 inline asm, but it is executed as x64 code
        ;// that's why it need sometimes REX_W() macro, right column contains detailed
        ;// transcription how it will be interpreted by CPU

        push   edi                  ;// push      rdi
        push   esi                  ;// push      rsi
                                    ;//           
        mov    edi, dstMem          ;// mov       edi, dword ptr [dstMem]       ; high part of RDI is zeroed
  REX_W mov    esi, _src.dw[0]      ;// mov       rsi, qword ptr [_src]
        mov    ecx, sz              ;// mov       ecx, dword ptr [sz]           ; high part of RCX is zeroed
                                    ;//           
        mov    eax, ecx             ;// mov       eax, ecx
        and    eax, 3               ;// and       eax, 3
        shr    ecx, 2               ;// shr       ecx, 2
                                    ;// 
        repe   cmpsd                ;// repe cmps dword ptr [rsi], dword ptr [rdi]
        jnz     _ret_false          ;// jnz       _ret_false
                                    ;// 
        test   eax, eax             ;// test      eax, eax
        je     _move_0              ;// je        _move_0
        cmp    eax, 1               ;// cmp       eax, 1
        je     _move_1              ;// je        _move_1
                                    ;// 
        cmpsw                       ;// cmps      word ptr [rsi], word ptr [rdi]
        jnz     _ret_false          ;// jnz       _ret_false
        cmp    eax, 2               ;// cmp       eax, 2
        je     _move_0              ;// je        _move_0
                                    ;// 
_move_1:                            ;// 
        cmpsb                       ;// cmps      byte ptr [rsi], byte ptr [rdi]
        jnz     _ret_false          ;// jnz       _ret_false
                                    ;// 
_move_0:                            ;// 
        mov    result, 1            ;// mov       byte ptr [result], 1
                                    ;// 
_ret_false:                         ;// 
        pop    esi                  ;// pop      rsi
        pop    edi                  ;// pop      rdi

        X64_End();
    }

    return result;
}

DWORD64 getTEB64()
{
    reg64 reg;
    reg.v = 0;
    
    X64_Start();
    // R12 register should always contain pointer to TEB64 in WoW64 processes
    X64_Push(_R12);
    // below pop will pop QWORD from stack, as we're in x64 mode now
    __asm pop reg.dw[0]
    X64_End();

    return reg.v;
}

DWORD64 __cdecl GetModuleHandle64(const wchar_t* lpModuleName)
{
	if (!g_isWow64)
		return 0;

    TEB64 teb64;
    getMem64(&teb64, getTEB64(), sizeof(TEB64));
    
    PEB64 peb64;
    getMem64(&peb64, teb64.ProcessEnvironmentBlock, sizeof(PEB64));
    PEB_LDR_DATA64 ldr;
    getMem64(&ldr, peb64.Ldr, sizeof(PEB_LDR_DATA64));

    DWORD64 LastEntry = peb64.Ldr + offsetof(PEB_LDR_DATA64, InLoadOrderModuleList);
    LDR_DATA_TABLE_ENTRY64 head;
    head.InLoadOrderLinks.Flink = ldr.InLoadOrderModuleList.Flink;
    do
    {
        getMem64(&head, head.InLoadOrderLinks.Flink, sizeof(LDR_DATA_TABLE_ENTRY64));

        wchar_t* tempBuf = (wchar_t*)malloc(head.BaseDllName.MaximumLength);
        if (nullptr == tempBuf)
            return 0;
        WATCH(tempBuf);
        getMem64(tempBuf, head.BaseDllName.Buffer, head.BaseDllName.MaximumLength);

        if (0 == _wcsicmp(lpModuleName, tempBuf))
            return head.DllBase;
    }
    while (head.InLoadOrderLinks.Flink != LastEntry);

    return 0;
}



DWORD64 __cdecl getNTDLL64()
{
    static DWORD64 ntdll64 = 0;
    if (0 != ntdll64)
        return ntdll64;

    ntdll64 = GetModuleHandle64(L"ntdll.dll");
    return ntdll64;
}

DWORD64 getLdrGetProcedureAddress()
{
    DWORD64 modBase = getNTDLL64();
	if (0 == modBase)
		return 0;
    
    IMAGE_DOS_HEADER idh;
    getMem64(&idh, modBase, sizeof(idh));

    IMAGE_NT_HEADERS64 inh;
    getMem64(&inh, modBase + idh.e_lfanew, sizeof(IMAGE_NT_HEADERS64));
    
    IMAGE_DATA_DIRECTORY& idd = inh.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    
    if (0 == idd.VirtualAddress)
        return 0;

    IMAGE_EXPORT_DIRECTORY ied;
    getMem64(&ied, modBase + idd.VirtualAddress, sizeof(ied));

    DWORD* rvaTable = (DWORD*)malloc(sizeof(DWORD)*ied.NumberOfFunctions);
    if (nullptr == rvaTable)
        return 0;
    WATCH(rvaTable);
    getMem64(rvaTable, modBase + ied.AddressOfFunctions, sizeof(DWORD)*ied.NumberOfFunctions);
    
    WORD* ordTable = (WORD*)malloc(sizeof(WORD)*ied.NumberOfFunctions);
    if (nullptr == ordTable)
        return 0;
    WATCH(ordTable);
    getMem64(ordTable, modBase + ied.AddressOfNameOrdinals, sizeof(WORD)*ied.NumberOfFunctions);

    DWORD* nameTable = (DWORD*)malloc(sizeof(DWORD)*ied.NumberOfNames);
    if (nullptr == nameTable)
        return 0;
    WATCH(nameTable);
    getMem64(nameTable, modBase + ied.AddressOfNames, sizeof(DWORD)*ied.NumberOfNames);

    //待优化: 使用二分法来加速搜索
    for (DWORD i = 0; i < ied.NumberOfFunctions; i++)
    {
        if (!cmpMem64((void*)"LdrGetProcedureAddress", modBase + nameTable[i], sizeof("LdrGetProcedureAddress")))
            continue;
        else
            return modBase + rvaTable[ordTable[i]];
    }
    return 0;
}

VOID __cdecl SetLastErrorFromX64Call(DWORD64 status)
{
	typedef ULONG (WINAPI *RtlNtStatusToDosError_t)(NTSTATUS Status);
	typedef ULONG (WINAPI *RtlSetLastWin32Error_t)(NTSTATUS Status);

	static RtlNtStatusToDosError_t RtlNtStatusToDosError = nullptr;
	static RtlSetLastWin32Error_t RtlSetLastWin32Error = nullptr;

	if ((nullptr == RtlNtStatusToDosError) || (nullptr == RtlSetLastWin32Error))
	{
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		RtlNtStatusToDosError = (RtlNtStatusToDosError_t)GetProcAddress(ntdll, "RtlNtStatusToDosError");
		RtlSetLastWin32Error = (RtlSetLastWin32Error_t)GetProcAddress(ntdll, "RtlSetLastWin32Error");
	}
	
	if ((nullptr != RtlNtStatusToDosError) && (nullptr != RtlSetLastWin32Error))
	{
		RtlSetLastWin32Error(RtlNtStatusToDosError((DWORD)status));
	}
}

DWORD64 __cdecl GetProcAddress64(DWORD64 hModule, const char* funcName)
{
    static DWORD64 _LdrGetProcedureAddress = 0;
    if (0 == _LdrGetProcedureAddress)
    {
        _LdrGetProcedureAddress = getLdrGetProcedureAddress();
        if (0 == _LdrGetProcedureAddress)
            return 0;
    }

    _UNICODE_STRING_T<DWORD64> fName = { 0 };
    fName.Buffer = (DWORD64)funcName;
    fName.Length = (WORD)strlen(funcName);
    fName.MaximumLength = fName.Length + 1;
    DWORD64 funcRet = 0;
    X64Call(_LdrGetProcedureAddress, 4, (DWORD64)hModule, (DWORD64)&fName, (DWORD64)0, (DWORD64)&funcRet);
    return funcRet;
}

SIZE_T __cdecl VirtualQueryEx64(HANDLE hProcess, DWORD64 lpAddress, MEMORY_BASIC_INFORMATION64* lpBuffer, SIZE_T dwLength)
{
    static DWORD64 ntqvm = 0;
    if (0 == ntqvm)
    {
        ntqvm = GetProcAddress64(getNTDLL64(), "NtQueryVirtualMemory");
        if (0 == ntqvm)
            return 0;
    }
    DWORD64 ret = 0;
    DWORD64 status = X64Call(ntqvm, 6, (DWORD64)hProcess, lpAddress, (DWORD64)0, (DWORD64)lpBuffer, (DWORD64)dwLength, (DWORD64)&ret);
	if (STATUS_SUCCESS != status)
		SetLastErrorFromX64Call(status);
	return (SIZE_T)ret;
}

DWORD64 __cdecl VirtualAllocEx64(HANDLE hProcess, DWORD64 lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    static DWORD64 ntavm = 0;
    if (0 == ntavm)
    {
        ntavm = GetProcAddress64(getNTDLL64(), "NtAllocateVirtualMemory");
        if (0 == ntavm)
            return 0;
    }

    DWORD64 tmpAddr = lpAddress;
    DWORD64 tmpSize = dwSize;
    DWORD64 ret = X64Call(ntavm, 6, (DWORD64)hProcess, (DWORD64)&tmpAddr, (DWORD64)0, (DWORD64)&tmpSize, (DWORD64)flAllocationType, (DWORD64)flProtect);
	if (STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
    else
        return tmpAddr;
}

BOOL __cdecl VirtualFreeEx64(HANDLE hProcess, DWORD64 lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    static DWORD64 ntfvm = 0;
    if (0 == ntfvm)
    {
        ntfvm = GetProcAddress64(getNTDLL64(), "NtFreeVirtualMemory");
        if (0 == ntfvm)
            return 0;
    }

    DWORD64 tmpAddr = lpAddress;
    DWORD64 tmpSize = dwSize;
    DWORD64 ret = X64Call(ntfvm, 4, (DWORD64)hProcess, (DWORD64)&tmpAddr, (DWORD64)&tmpSize, (DWORD64)dwFreeType);
	if (STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
    else
        return TRUE;
}

BOOL __cdecl VirtualProtectEx64(HANDLE hProcess, DWORD64 lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect)
{
    static DWORD64 ntpvm = 0;
    if (0 == ntpvm)
    {
        ntpvm = GetProcAddress64(getNTDLL64(), "NtProtectVirtualMemory");
        if (0 == ntpvm)
            return 0;
    }

    DWORD64 tmpAddr = lpAddress;
    DWORD64 tmpSize = dwSize;
    DWORD64 ret = X64Call(ntpvm, 5, (DWORD64)hProcess, (DWORD64)&tmpAddr, (DWORD64)&tmpSize, (DWORD64)flNewProtect, (DWORD64)lpflOldProtect);
	if (STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
    else
        return TRUE;
}

BOOL __cdecl ReadProcessMemory64(HANDLE hProcess, DWORD64 lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesRead)
{
    static DWORD64 nrvm = 0;
    if (0 == nrvm)
    {
        nrvm = GetProcAddress64(getNTDLL64(), "NtReadVirtualMemory");
        if (0 == nrvm)
            return 0;
    }
    DWORD64 numOfBytes = lpNumberOfBytesRead ? *lpNumberOfBytesRead : 0;
    DWORD64 ret = X64Call(nrvm, 5, (DWORD64)hProcess, lpBaseAddress, (DWORD64)lpBuffer, (DWORD64)nSize, (DWORD64)&numOfBytes);
	if (STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
    else
    {
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = (SIZE_T)numOfBytes;
        return TRUE;
    }
}

BOOL __cdecl WriteProcessMemory64(HANDLE hProcess, DWORD64 lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T *lpNumberOfBytesWritten)
{
    static DWORD64 nrvm = 0;
    if (0 == nrvm)
    {
        nrvm = GetProcAddress64(getNTDLL64(), "NtWriteVirtualMemory");
        if (0 == nrvm)
            return 0;
    }
    DWORD64 numOfBytes = lpNumberOfBytesWritten ? *lpNumberOfBytesWritten : 0;
    DWORD64 ret = X64Call(nrvm, 5, (DWORD64)hProcess, lpBaseAddress, (DWORD64)lpBuffer, (DWORD64)nSize, (DWORD64)&numOfBytes);
	if (STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
    else
    {
        if (lpNumberOfBytesWritten)
            *lpNumberOfBytesWritten = (SIZE_T)numOfBytes;
        return TRUE;
    }
}

BOOL __cdecl GetThreadContext64(HANDLE hThread, _CONTEXT64* lpContext)
{
    static DWORD64 gtc = 0;
    if (0 == gtc)
    {
        gtc = GetProcAddress64(getNTDLL64(), "NtGetContextThread");
        if (0 == gtc)
            return 0;
    }
    DWORD64 ret = X64Call(gtc, 2, (DWORD64)hThread, (DWORD64)lpContext);
	if(STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
    else
        return TRUE;
}

BOOL __cdecl SetThreadContext64(HANDLE hThread, _CONTEXT64* lpContext)
{
    static DWORD64 stc = 0;
    if (0 == stc)
    {
        stc = GetProcAddress64(getNTDLL64(), "NtSetContextThread");
        if (0 == stc)
            return 0;
    }
    DWORD64 ret = X64Call(stc, 2, (DWORD64)hThread, (DWORD64)lpContext);
	if (STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
    else
        return TRUE;
}

HMODULE __cdecl getNTDLL32()
{
	static HMODULE ntdll32 = 0;
	if (0 != ntdll32)
		return ntdll32;

	ntdll32 = GetModuleHandle(TEXT("ntdll.dll"));
	return ntdll32;
}

typedef NTSTATUS(NTAPI* fnNtCreateThreadEx)(
	OUT PHANDLE hThread,
	IN ACCESS_MASK DesiredAccess,
	IN LPVOID ObjectAttributes,
	IN HANDLE ProcessHandle,
	IN LPTHREAD_START_ROUTINE lpStartAddress,
	IN LPVOID lpParameter,
	IN DWORD Flags,
	IN SIZE_T StackZeroBits,
	IN SIZE_T SizeOfStackCommit,
	IN SIZE_T SizeOfStackReserve,
	OUT LPVOID lpBytesBuffer
	);
BOOL __cdecl NtCreateThreadEx(HANDLE& hThread, HANDLE hProcess, PTHREAD_START_ROUTINE lpEntryFunc, LPVOID lpArg, CreateThreadFlags flags, DWORD access)
{
	static fnNtCreateThreadEx ntctex = 0;
	if (0 == ntctex)
	{
		ntctex = (fnNtCreateThreadEx)GetProcAddress(getNTDLL32(), "NtCreateThreadEx");
		if (0 == ntctex)
			return 0;
	}
	
	return STATUS_SUCCESS == ntctex(&hThread, access, NULL, hProcess, lpEntryFunc, lpArg, flags, 0, 0x1000, 0x100000, NULL);
}

BOOL __cdecl NtCreateThreadEx64(HANDLE& hThread, HANDLE hProcess, DWORD64 lpEntryFunc, DWORD64 lpArg, CreateThreadFlags flags, DWORD access)
{
	static DWORD64 ntctex64 = 0;
	if (0 == ntctex64)
	{
		ntctex64 = GetProcAddress64(getNTDLL64(), "NtCreateThreadEx");
		if (0 == ntctex64)
			return 0;
	}
	
	DWORD64 hThd2 = NULL;
	DWORD64 ret = X64Call(ntctex64, 11, (DWORD64)&hThd2, (DWORD64)access, 0ull,
		(DWORD64)hProcess, (DWORD64)lpEntryFunc, (DWORD64)lpArg, (DWORD64)flags,
		0ull, 0x1000ull, 0x100000ull, 0ull
	);

	if (STATUS_SUCCESS != ret)
	{
		SetLastErrorFromX64Call(ret);
		return FALSE;
	}
	else
	{
		hThread = reinterpret_cast<HANDLE>(hThd2);
		return TRUE;
	}
}

typedef NTSTATUS(NTAPI* fnNtQueueApcThread)(
	IN HANDLE ThreadHandle,
	IN PVOID ApcRoutine, /*PKNORMAL_ROUTINE*/
	IN PVOID NormalContext,
	IN PVOID SystemArgument1,
	IN PVOID SystemArgument2
	);
BOOL __cdecl NtQueueUserApc(HANDLE hThread, LPVOID func, LPVOID arg1, LPVOID arg2, LPVOID arg3)
{
	static fnNtQueueApcThread ntapc = 0;
	if (0 == ntapc)
	{
		ntapc = (fnNtQueueApcThread)GetProcAddress(getNTDLL32(), "NtQueueApcThread");
		if (0 == ntapc)
			return 0;
	}

	return STATUS_SUCCESS == ntapc(hThread, func, arg1, arg2, arg3);
}

BOOL __cdecl NtQueueUserApc64(HANDLE hThread, DWORD64 func, DWORD64 arg1, DWORD64 arg2, DWORD64 arg3)
{
	static DWORD64 ntapc64 = 0;
	if (0 == ntapc64)
	{
		ntapc64 = GetProcAddress64(getNTDLL64(), "NtQueueApcThread");
		if (ntapc64 == 0)
			return 0;
	}

	return STATUS_SUCCESS == X64Call(ntapc64, 5, (DWORD64)hThread, func, arg1, arg2, arg3);
}

typedef NTSTATUS(NTAPI* fnNtQueryInformationThread)(
	IN HANDLE ThreadHandle,
	IN THREADINFOCLASS ThreadInformationClass,
	OUT PVOID ThreadInformation,
	IN ULONG ThreadInformationLength,
	OUT PULONG ReturnLength OPTIONAL);
BOOL __cdecl NtQueryInformationThread(HANDLE hThread, THREADINFOCLASS threadInfoClass,
	PVOID threadInfo, ULONG threadInfoLen, PULONG returnLen)
{
	static fnNtQueryInformationThread ntqit = 0;
	if (0 == ntqit)
	{
		ntqit = (fnNtQueryInformationThread)GetProcAddress(getNTDLL32(), "NtQueryInformationThread");
		if (0 == ntqit)
			return 0;
	}

	return STATUS_SUCCESS == ntqit(hThread, threadInfoClass, threadInfo, threadInfoLen, returnLen);
}

BOOL __cdecl NtQueryInformationThread64(HANDLE hThread, THREADINFOCLASS threadInfoClass,
	PVOID threadInfo, ULONG threadInfoLen, PULONG returnLen)
{
	static DWORD64 ntqit64 = 0;
	if (0 == ntqit64)
	{
		ntqit64 = GetProcAddress64(getNTDLL64(), "NtQueryInformationThread");
		if (0 == ntqit64)
			return 0;
	}

	unsigned char localBuffer[100];
	DWORD64 ret = X64Call(ntqit64, 5, (DWORD64)hThread, (DWORD64)threadInfoClass, (DWORD64)localBuffer, (DWORD64)threadInfoLen, (DWORD64)returnLen);
	memcpy(threadInfo, localBuffer, threadInfoLen);
	return STATUS_SUCCESS == ret;
}


typedef NTSTATUS(NTAPI* fnNtQueryInformationProcess)(
	IN HANDLE ProcessHandle,
	IN PROCESSINFOCLASS ProcessInformationClass,
	OUT PVOID ProcessInformation,
	IN ULONG ProcessInformationLength,
	OUT PULONG ReturnLength OPTIONAL);
BOOL __cdecl NtQueryInformationProcess(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass, 
	PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength)
{
	static fnNtQueryInformationProcess ntqip = 0;
	if (0 == ntqip)
	{
		ntqip = (fnNtQueryInformationProcess)GetProcAddress(getNTDLL32(), "NtQueryInformationProcess");
		if (0 == ntqip)
			return 0;
	}

	return STATUS_SUCCESS == ntqip(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);
}

BOOL __cdecl NtQueryInformationProcess64(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass,
	PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength)
{
	static DWORD64 ntqip64 = 0;
	if (0 == ntqip64)
	{
		ntqip64 = GetProcAddress64(getNTDLL64(), "NtQueryInformationProcess");
		if (0 == ntqip64)
			return 0;
	}

	unsigned char localBuffer[100];
	DWORD64 ret = X64Call(ntqip64, 5, (DWORD64)ProcessHandle, (DWORD64)ProcessInformationClass, (DWORD64)localBuffer, (DWORD64)ProcessInformationLength, (DWORD64)ReturnLength);
	memcpy(ProcessInformation, localBuffer, ProcessInformationLength);
	return STATUS_SUCCESS == ret;
}

typedef NTSTATUS(NTAPI *fnNtWow64QueryInformationProcess64)(
	IN  HANDLE ProcessHandle,
	IN  ULONG  ProcessInformationClass,
	OUT PVOID  ProcessInformation64,
	IN  ULONG  Length,
	OUT PULONG ReturnLength OPTIONAL
	);
BOOL __cdecl NtWow64QueryInformationProcess64(
	IN  HANDLE ProcessHandle,
	IN  ULONG  ProcessInformationClass,
	OUT PVOID  ProcessInformation64,
	IN  ULONG  Length,
	OUT PULONG ReturnLength OPTIONAL)
{
	static fnNtWow64QueryInformationProcess64 ntqipwow64 = 0;
	if (0 == ntqipwow64)
	{
		ntqipwow64 = (fnNtWow64QueryInformationProcess64)GetProcAddress(getNTDLL32(), "NtWow64QueryInformationProcess64");
		if (0 == ntqipwow64)
			return 0;
	}

	return STATUS_SUCCESS == ntqipwow64(ProcessHandle, ProcessInformationClass, ProcessInformation64, Length, ReturnLength);
}