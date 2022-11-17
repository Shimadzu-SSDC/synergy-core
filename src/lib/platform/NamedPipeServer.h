#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <memory>
#include <exception>

namespace SNP
{

    template<int Instances = 4, int MaxBufSize = 4096, int PipeTimeout = 5000>
    class NPServer
    {
    private:

        enum EPipeState
        {
            PSConnecting = 0,
            PSReading,
            PSWriting
        };

        typedef struct
        {
            OVERLAPPED oOverlap;
            HANDLE hPipeInst;
            uint8_t chRequest[MaxBufSize];
            DWORD cbRead;
            uint8_t chReply[MaxBufSize];
            DWORD cbToWrite;
            EPipeState dwState;
            bool fPendingIO;
        } PIPEINST, * LPPIPEINST;

    public:

        inline NPServer(const char* pipeName)
        {
            m_PipeName = std::string("\\\\.\\pipe\\") + pipeName;
            m_Thread = INVALID_HANDLE_VALUE;
            m_StopRequested = false;
            m_psd = nullptr;
        }

        inline virtual ~NPServer()
        {
            _ASSERT((!IsServerStarted()) && "Server must be stopped before destructuring this object");
        }

        inline bool IsServerStarted() { return m_Thread != INVALID_HANDLE_VALUE; }

        inline void StartServer()
        {
            if (m_Thread == INVALID_HANDLE_VALUE)
            {
                m_StopRequested = false;
                m_Thread = CreateThread(nullptr, 0, ThreadProcS, this, 0, nullptr);
            }
        }

        inline void StopServer()
        {
            if (m_Thread != INVALID_HANDLE_VALUE)
            {
                m_StopRequested = true;
                SetEvent(m_Events[0]);
                WaitForSingleObject(m_Thread, INFINITE);
                CloseHandle(m_Thread);
                m_Thread = INVALID_HANDLE_VALUE;
            }
        }

    protected:

        virtual void GetAnswerToRequest(const uint8_t* requestBuffer, DWORD requestSize, DWORD maxResponseSize, uint8_t* responseBuffer, DWORD& responseSize) = 0;

        virtual void OnServerFatalError(const char* what) = 0;

    private:


        inline static DWORD ThreadProcS(void* lParam)
        {
            ((NPServer*)lParam)->ThreadProc();
            return 0;
        }

        inline void ThreadProc()
        {
            try
            {

                InitializeAllPipes();

                while (!m_StopRequested)
                {
                    ProcessPipeEvents();
                }

            }
            catch (std::exception& ex)
            {
                OnServerFatalError(ex.what());
            }

            CloseAllPipes();
        }

        inline void InitializeAllPipes()
        {
            for (int i = 0; i < Instances; i++)
            {
                m_Events[i] = INVALID_HANDLE_VALUE;
                m_Pipes[i].hPipeInst = INVALID_HANDLE_VALUE;
            }

            m_psd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
            if ((!m_psd) ||
                (!InitializeSecurityDescriptor(m_psd, SECURITY_DESCRIPTOR_REVISION)) ||
                (!SetSecurityDescriptorDacl(m_psd, TRUE, NULL, FALSE)))
            {
                throw std::exception("Failed to create or initialize the security descriptor.");
            }

            SECURITY_ATTRIBUTES sa;
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = m_psd;
            sa.bInheritHandle = FALSE;

            for (int i = 0; i < Instances; i++)
            {
                m_Events[i] = CreateEvent(nullptr, TRUE, TRUE, nullptr);

                m_Pipes[i].oOverlap.hEvent = m_Events[i];
                m_Pipes[i].oOverlap.Offset = 0;
                m_Pipes[i].oOverlap.OffsetHigh = 0;
                m_Pipes[i].fPendingIO = false;
                m_Pipes[i].dwState = PSConnecting;

                m_Pipes[i].hPipeInst = CreateNamedPipeA(m_PipeName.c_str(),
                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                    (DWORD)Instances,
                    (DWORD)MaxBufSize,
                    (DWORD)MaxBufSize,
                    (DWORD)PipeTimeout,
                    &sa);

                if (m_Pipes[i].hPipeInst == INVALID_HANDLE_VALUE)
                    throw std::exception("CreateNamedPipe creation failed.");

                ConnectToNewClient(m_Pipes[i]);
            }
        }

        inline void CloseAllPipes()
        {
            for (int i = 0; i < Instances; i++)
            {
                if (m_Pipes[i].hPipeInst != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(m_Pipes[i].hPipeInst);
                    m_Pipes[i].hPipeInst = INVALID_HANDLE_VALUE;
                }
                if (m_Events[i] != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(m_Events[i]);
                    m_Events[i] = INVALID_HANDLE_VALUE;
                }
            }

            if (m_psd)
            {
                LocalFree(m_psd);
                m_psd = nullptr;
            }
        }

        inline void ProcessPipeEvents()
        {
            DWORD waitResult = WaitForMultipleObjects(Instances, m_Events, FALSE, INFINITE);
            if (m_StopRequested)
                return;

            PIPEINST& pipe = m_Pipes[waitResult - WAIT_OBJECT_0];
            bool mustWaitAgain = false;
            if (pipe.fPendingIO)
            {
                DWORD cbRet = 0;
                BOOL overlappedSucceeded = GetOverlappedResult(pipe.hPipeInst, &(pipe.oOverlap), &cbRet, FALSE);

                switch (pipe.dwState)
                {
                case PSConnecting:
                    if (!overlappedSucceeded)
                        throw std::exception("Unexpected connection error.");

                    pipe.dwState = PSReading;
                    break;

                case PSReading:
                    if (!overlappedSucceeded || cbRet == 0)
                    {
                        DisconnectAndReconnect(pipe);
                        mustWaitAgain = true;
                    }
                    else
                    {
                        pipe.cbRead = cbRet;
                        pipe.dwState = PSWriting;
                    }
                    break;

                case PSWriting:
                    if (!overlappedSucceeded || cbRet != pipe.cbToWrite)
                    {
                        DisconnectAndReconnect(pipe);
                        mustWaitAgain = true;
                    }
                    else
                    {
                        pipe.dwState = PSReading;
                    }
                    break;
                }
            }

            if (!mustWaitAgain)
            {
                switch (pipe.dwState)
                {
                case PSReading:
                {
                    BOOL readSucceeded = ReadFile(pipe.hPipeInst, pipe.chRequest, (DWORD)MaxBufSize, &pipe.cbRead, &pipe.oOverlap);
                    if (readSucceeded && pipe.cbRead != 0)
                    {
                        pipe.fPendingIO = false;
                        pipe.dwState = PSWriting;
                    }
                    else if (!readSucceeded && GetLastError() == ERROR_IO_PENDING)
                    {
                        pipe.fPendingIO = true;
                    }
                    else
                    {
                        DisconnectAndReconnect(pipe);
                    }
                }
                break;

                case PSWriting:
                {
                    pipe.cbToWrite = 0;
                    GetAnswerToRequest(pipe.chRequest, pipe.cbRead, (DWORD)MaxBufSize, pipe.chReply, pipe.cbToWrite);

                    DWORD cbRet = 0;
                    BOOL writeSucceeded = WriteFile(pipe.hPipeInst, pipe.chReply, pipe.cbToWrite, &cbRet, &pipe.oOverlap);

                    if (writeSucceeded && cbRet == pipe.cbToWrite)
                    {
                        pipe.fPendingIO = false;
                        pipe.dwState = PSReading;
                    }
                    else if (!writeSucceeded && GetLastError() == ERROR_IO_PENDING)
                    {
                        pipe.fPendingIO = TRUE;
                    }
                    else
                    {
                        DisconnectAndReconnect(pipe);
                    }
                }
                break;
                };
            }
        }

        void DisconnectAndReconnect(PIPEINST& inst)
        {
            DisconnectNamedPipe(inst.hPipeInst);
            ConnectToNewClient(inst);
        }

        void ConnectToNewClient(PIPEINST& inst)
        {
            inst.fPendingIO = true;
            inst.dwState = PSConnecting;
            if (ConnectNamedPipe(inst.hPipeInst, &(inst.oOverlap)))
                throw std::exception("ConnectNamedPipe returned unexpected connected result.");

            switch (GetLastError())
            {
            case ERROR_IO_PENDING:
                break;

            case ERROR_PIPE_CONNECTED:
                inst.fPendingIO = false;
                inst.dwState = PSReading;
                SetEvent(inst.oOverlap.hEvent);
                break;

            default:
                throw std::exception("ConnectNamedPipe generated unexpected error code.");
            }
        }

    private:

        HANDLE m_Events[Instances];
        PIPEINST m_Pipes[Instances];
        std::string m_PipeName;
        HANDLE m_Thread;
        bool m_StopRequested = false;
        PSECURITY_DESCRIPTOR m_psd;

    };

}