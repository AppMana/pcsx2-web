// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/HostSys.h"

#include <cerrno>
#include <cstdio>
#include <unistd.h>

#include "fmt/format.h"

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	const unsigned pid = static_cast<unsigned>(getpid());
	return fmt::format("{}_{}", prefix, pid);
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	std::fprintf(stderr, "CreateSharedMemory(%s, %zu) is not supported on wasm32\n", name, size);
	return nullptr;
}

void HostSys::DestroySharedMemory(void* ptr)
{
}

size_t HostSys::GetRuntimePageSize()
{
	const long res = sysconf(_SC_PAGESIZE);
	return (res > 0) ? static_cast<size_t>(res) : 0;
}

size_t HostSys::GetRuntimeCacheLineSize()
{
	return __cachelinesize;
}

SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "No mappings left");
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit)
{
	return nullptr;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	return nullptr;
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	return false;
}

bool PageFaultHandler::Install(Error* error)
{
	return true;
}

bool PageFaultHandler::InstallSecondaryThread()
{
	return true;
}
