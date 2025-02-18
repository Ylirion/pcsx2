/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */


#include "PrecompiledHeader.h"
#include "IopCommon.h"
#include "SaveState.h"

#include "ps2/BiosTools.h"
#include "COP0.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "Cache.h"
#include "gui/AppConfig.h"

#include "Elfheader.h"
#include "Counters.h"

#include "common/SafeArray.inl"
#include "SPU2/spu2.h"
#include "gui/ConsoleLogger.h"

using namespace R5900;


static void PreLoadPrep()
{
	SysClearExecutionCache();
}

static void PostLoadPrep()
{
	resetCache();
//	WriteCP0Status(cpuRegs.CP0.n.Status.val);
	for(int i=0; i<48; i++) MapTLB(i);
	if (EmuConfig.Gamefixes.GoemonTlbHack) GoemonPreloadTlb();

	UpdateVSyncRate();
}

// --------------------------------------------------------------------------------------
//  SaveStateBase  (implementations)
// --------------------------------------------------------------------------------------
wxString SaveStateBase::GetFilename( int slot )
{
	wxString serialName( DiscSerial );
	if (serialName.IsEmpty()) serialName = L"BIOS";

	return (g_Conf->Folders.Savestates +
		pxsFmt( L"%s (%08X).%02d.p2s", WX_STR(serialName), ElfCRC, slot )).GetFullPath();

	//return (g_Conf->Folders.Savestates +
	//	pxsFmt( L"%08X.%03d", ElfCRC, slot )).GetFullPath();
}

SaveStateBase::SaveStateBase( SafeArray<u8>& memblock )
{
	Init( &memblock );
}

SaveStateBase::SaveStateBase( SafeArray<u8>* memblock )
{
	Init( memblock );
}

void SaveStateBase::Init( SafeArray<u8>* memblock )
{
	m_memory	= memblock;
	m_version	= g_SaveVersion;
	m_idx		= 0;
}

void SaveStateBase::PrepBlock( int size )
{
	pxAssertDev( m_memory, "Savestate memory/buffer pointer is null!" );

	const int end = m_idx+size;
	if( IsSaving() )
		m_memory->MakeRoomFor( end );
	else
	{
		if( m_memory->GetSizeInBytes() < end )
			throw Exception::SaveStateLoadError();
	}
}

void SaveStateBase::FreezeTag( const char* src )
{
	const uint allowedlen = sizeof( m_tagspace )-1;
	pxAssertDev( strlen(src) < allowedlen, pxsFmt( L"Tag name exceeds the allowed length of %d chars.", allowedlen) );

	memzero( m_tagspace );
	strcpy( m_tagspace, src );
	Freeze( m_tagspace );

	if( strcmp( m_tagspace, src ) != 0 )
	{
		wxString msg( L"Savestate data corruption detected while reading tag: " + fromUTF8(src) );
		pxFail( msg );
		throw Exception::SaveStateLoadError().SetDiagMsg(msg);
	}
}

SaveStateBase& SaveStateBase::FreezeBios()
{
	FreezeTag( "BIOS" );

	// Check the BIOS, and issue a warning if the bios for this state
	// doesn't match the bios currently being used (chances are it'll still
	// work fine, but some games are very picky).
	
	u32 bioscheck = BiosChecksum;
	char biosdesc[256];

	pxToUTF8 utf8(BiosDescription);

	memzero( biosdesc );
	memcpy( biosdesc, utf8, std::min( sizeof(biosdesc), utf8.Length() ) );
	
	Freeze( bioscheck );
	Freeze( biosdesc );

	if (bioscheck != BiosChecksum)
	{
		Console.Newline();
		Console.Indent(1).Error( "Warning: BIOS Version Mismatch, savestate may be unstable!" );
		Console.Indent(2).Error(
			"Current BIOS:   %ls (crc=0x%08x)\n"
			"Savestate BIOS: %s (crc=0x%08x)\n",
			BiosDescription.wx_str(), BiosChecksum,
			biosdesc, bioscheck
		);
	}
	
	return *this;
}

static const uint MainMemorySizeInBytes =
	Ps2MemSize::MainRam	+ Ps2MemSize::Scratch		+ Ps2MemSize::Hardware +
	Ps2MemSize::IopRam	+ Ps2MemSize::IopHardware;

SaveStateBase& SaveStateBase::FreezeMainMemory()
{
	vu1Thread.WaitVU(); // Finish VU1 just in-case...
	if (IsLoading()) PreLoadPrep();
	else m_memory->MakeRoomFor( m_idx + MainMemorySizeInBytes );

	// First Block - Memory Dumps
	// ---------------------------
	FreezeMem(eeMem->Main,		Ps2MemSize::MainRam);		// 32 MB main memory
	FreezeMem(eeMem->Scratch,	Ps2MemSize::Scratch);		// scratch pad
	FreezeMem(eeHw,				Ps2MemSize::Hardware);		// hardware memory

	FreezeMem(iopMem->Main, 	Ps2MemSize::IopRam);		// 2 MB main memory
	FreezeMem(iopHw,			Ps2MemSize::IopHardware);	// hardware memory
	
	FreezeMem(vuRegs[0].Micro,	VU0_PROGSIZE);
	FreezeMem(vuRegs[0].Mem,	VU0_MEMSIZE);

	FreezeMem(vuRegs[1].Micro,	VU1_PROGSIZE);
	FreezeMem(vuRegs[1].Mem,	VU1_MEMSIZE);
	
	return *this;
}

SaveStateBase& SaveStateBase::FreezeInternals()
{
	vu1Thread.WaitVU(); // Finish VU1 just in-case...
	// Print this until the MTVU problem in gifPathFreeze is taken care of (rama)
	if (THREAD_VU1) Console.Warning("MTVU speedhack is enabled, saved states may not be stable");
	
	if (IsLoading()) PreLoadPrep();

	// Second Block - Various CPU Registers and States
	// -----------------------------------------------
	FreezeTag( "cpuRegs" );
	Freeze(cpuRegs);		// cpu regs + COP0
	Freeze(psxRegs);		// iop regs
	Freeze(fpuRegs);
	Freeze(tlb);			// tlbs

	// Third Block - Cycle Timers and Events
	// -------------------------------------
	FreezeTag( "Cycles" );
	Freeze(EEsCycle);
	Freeze(EEoCycle);
	Freeze(g_nextEventCycle);
	Freeze(g_iopNextEventCycle);
	Freeze(s_iLastCOP0Cycle);
	Freeze(s_iLastPERFCycle);

	// Fourth Block - EE-related systems
	// ---------------------------------
	FreezeTag( "EE-Subsystems" );
	rcntFreeze();
	gsFreeze();
	vuMicroFreeze();
	vif0Freeze();
	vif1Freeze();
	sifFreeze();
	ipuFreeze();
	ipuDmaFreeze();
	gifFreeze();
	gifDmaFreeze();
	sprFreeze();
	mtvuFreeze();

	// Fifth Block - iop-related systems
	// ---------------------------------
	FreezeTag( "IOP-Subsystems" );
	FreezeMem(iopMem->Sif, sizeof(iopMem->Sif));		// iop's sif memory (not really needed, but oh well)

	psxRcntFreeze();
	sioFreeze();
	sio2Freeze();
	cdrFreeze();
	cdvdFreeze();

	
	// technically this is HLE BIOS territory, but we don't have enough such stuff
	// to merit an HLE Bios sub-section... yet.
	deci2Freeze();

	InputRecordingFreeze();

	if( IsLoading() )
		PostLoadPrep();

	return *this;
}


// --------------------------------------------------------------------------------------
//  memSavingState (implementations)
// --------------------------------------------------------------------------------------
// uncompressed to/from memory state saves implementation

memSavingState::memSavingState( SafeArray<u8>& save_to )
	: SaveStateBase( save_to )
{
}

memSavingState::memSavingState( SafeArray<u8>* save_to )
	: SaveStateBase( save_to )
{
}

// Saving of state data
void memSavingState::FreezeMem( void* data, int size )
{
	if (!size) return;

	m_memory->MakeRoomFor( m_idx + size );
	memcpy( m_memory->GetPtr(m_idx), data, size );
	m_idx += size;
}

void memSavingState::MakeRoomForData()
{
	pxAssertDev( m_memory, "Savestate memory/buffer pointer is null!" );

	m_memory->ChunkSize = ReallocThreshold;
	m_memory->MakeRoomFor( m_idx + MemoryBaseAllocSize );
}

// --------------------------------------------------------------------------------------
//  memLoadingState  (implementations)
// --------------------------------------------------------------------------------------
memLoadingState::memLoadingState( const SafeArray<u8>& load_from )
	: SaveStateBase( const_cast<SafeArray<u8>&>(load_from) )
{
}

memLoadingState::memLoadingState( const SafeArray<u8>* load_from )
	: SaveStateBase( const_cast<SafeArray<u8>*>(load_from) )
{
}

// Loading of state data from a memory buffer...
void memLoadingState::FreezeMem( void* data, int size )
{
	const u8* const src = m_memory->GetPtr(m_idx);
	m_idx += size;
	memcpy( data, src, size );
}

wxString Exception::SaveStateLoadError::FormatDiagnosticMessage() const
{
	FastFormatUnicode retval;
	retval.Write("Savestate is corrupt or incomplete!\n");
	OSDlog(Color_Red, false, "Error: Savestate is corrupt or incomplete!");
	_formatDiagMsg(retval);
	return retval;
}

wxString Exception::SaveStateLoadError::FormatDisplayMessage() const
{
	FastFormatUnicode retval;
	retval.Write(_("The savestate cannot be loaded, as it appears to be corrupt or incomplete."));
	retval.Write("\n");
	OSDlog(Color_Red, false, "Error: The savestate cannot be loaded, as it appears to be corrupt or incomplete.");
	_formatUserMsg(retval);
	return retval;
}
