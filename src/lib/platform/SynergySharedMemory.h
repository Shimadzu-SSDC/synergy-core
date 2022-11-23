#pragma once

#include <windows.h>

namespace SNP
{

    class SynergySharedMemory
    {

    private:

#pragma pack(push, 1)
        struct SSharedMemory
        {
            SSharedMemory()
            {
                CurrentHostActive = 1L;
            }

            volatile long CurrentHostActive;
        };
#pragma pack(pop)

    public:

        inline SynergySharedMemory()
            : m_psd(nullptr), m_hFM(INVALID_HANDLE_VALUE), m_Mapping(nullptr)
        {
            m_psd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
            if ((!m_psd) ||
                (!InitializeSecurityDescriptor(m_psd, SECURITY_DESCRIPTOR_REVISION)) ||
                (!SetSecurityDescriptorDacl(m_psd, TRUE, NULL, FALSE)))
                return;

            SECURITY_ATTRIBUTES sa;
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = m_psd;
            sa.bInheritHandle = FALSE;

            m_hFM = CreateFileMappingA(
                INVALID_HANDLE_VALUE,
                &sa,
                PAGE_READWRITE,
                0,
                (WORD)sizeof(SSharedMemory),
                "Global\\SynergyStateMapping"); 

            if (m_hFM == INVALID_HANDLE_VALUE)
                return;

            bool alreadyCreated = (GetLastError() == ERROR_ALREADY_EXISTS);
            m_Mapping = (SSharedMemory*)MapViewOfFile(m_hFM, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SSharedMemory));
            if (m_Mapping && !alreadyCreated)
            {
                *m_Mapping = SSharedMemory();
            }
        }

        inline ~SynergySharedMemory()
        {
            if (m_Mapping)
            {
                UnmapViewOfFile(m_Mapping);
                m_Mapping = nullptr;
            }

            if (m_hFM != INVALID_HANDLE_VALUE)
            {
                CloseHandle(m_hFM);
                m_hFM = INVALID_HANDLE_VALUE;
            }

            if (m_psd)
            {
                LocalFree(m_psd);
                m_psd = nullptr;
            }
        }

        inline bool OK() { return m_Mapping != nullptr; }

        inline bool IsCurrentHostActive() { return OK() ? InterlockedCompareExchange(&((*m_Mapping).CurrentHostActive), 0L, 0L) != 0L : SSharedMemory().CurrentHostActive != 0L; }

        inline void SetHostActive(bool active) 
        {
            if (OK())
                InterlockedExchange(&((*m_Mapping).CurrentHostActive), active ? 1L : 0L);
        }

    private:

        HANDLE               m_hFM;
        SSharedMemory*       m_Mapping;
        PSECURITY_DESCRIPTOR m_psd;

    };


}
