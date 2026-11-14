// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "config/config.h"
#include "cpu/cpu.h"
#include "cpu/registers.h"
#include "dosbox_test_fixture.h"
#include "hardware/memory.h"

namespace {

class Cpu8088Test : public DOSBoxTestFixture {
public:
	void SetUp() override
	{
		DOSBoxTestFixture::SetUp();
		configure("8088");
		CPU_SET_CRX(0, 0);
		CPU_SetSegGeneral(cs, 0x1000);
		CPU_SetSegGeneral(ds, 0x3000);
		CPU_SetSegGeneral(ss, 0x2000);
		cpu.code.big      = false;
		cpu.stack.big     = false;
		cpu.stack.mask    = 0xffff;
		cpu.stack.notmask = 0xffff0000;
		reg_eip           = 0x100;
		reg_esp           = 0x1000;
		CPU_SetFlags(0, FMASK_ALL);
	}

	void TearDown() override
	{
		// CPU state outlives individual fixtures.
		configure("auto");
		CPU_SetSegGeneral(cs, 0);
		CPU_SetSegGeneral(ds, 0);
		CPU_SetSegGeneral(ss, 0);
		CPU_SetFlags(FLAG_IF, FMASK_ALL);
		DOSBoxTestFixture::TearDown();
	}

protected:
	void configure(const char* type, const char* core = "normal")
	{
		set_section_property_value("cpu", "core", core);
		set_section_property_value("cpu", "cputype", type);
		CPU_Init();
	}

	void code(const std::span<const uint8_t> bytes)
	{
		auto address = SegPhys(cs) + reg_ip;
		for (const auto byte : bytes) {
			mem_writeb(address++, byte);
		}
	}

	void run(const int cycles = 1)
	{
		CPU_Cycles = cycles;
		cpudecoder();
	}
};

TEST_F(Cpu8088Test, PopCsPreservesNextIpWithNewSegment)
{
	code(std::array<uint8_t, 1>{0x0f});
	CPU_Push16(0x4000);
	run();
	EXPECT_EQ(SegValue(cs), 0x4000);
	EXPECT_EQ(SegPhys(cs), 0x40000u);
	EXPECT_EQ(SegValue(ds), 0x3000);
	EXPECT_EQ(reg_ip, 0x101);
	EXPECT_EQ(reg_sp, 0x1000);
}

TEST_F(Cpu8088Test, LodeRunnerSegmentRestoreSequence)
{
	// PUSH AX; PUSH CS; POP CS; POP AX, with the original single-byte 0Fh.
	code(std::array<uint8_t, 4>{0x50, 0x0e, 0x0f, 0x58});
	reg_ax = 0x1234;
	run(3);
	reg_ax = 0;
	run();
	EXPECT_EQ(SegValue(cs), 0x1000);
	EXPECT_EQ(reg_ax, 0x1234);
	EXPECT_EQ(reg_ip, 0x104);
	EXPECT_EQ(reg_sp, 0x1000);
}

TEST_F(Cpu8088Test, PopCsWrapsStackPointer)
{
	code(std::array<uint8_t, 1>{0x0f});
	reg_sp = 0xfffe;
	mem_writew(SegPhys(ss) + reg_sp, 0x1000);
	run();
	EXPECT_EQ(reg_sp, 0);
	EXPECT_EQ(reg_ip, 0x101);
}

TEST_F(Cpu8088Test, PopCsWrapsInstructionPointer)
{
	reg_ip = 0xffff;
	code(std::array<uint8_t, 1>{0x0f});
	CPU_Push16(0x4000);
	run();
	EXPECT_EQ(SegValue(cs), 0x4000);
	EXPECT_EQ(reg_ip, 0);
}

TEST_F(Cpu8088Test, InstructionImmediateWrapsAtSegmentEnd)
{
	reg_ip = 0xffff;
	code(std::array<uint8_t, 1>{0xb8}); // MOV AX,1234h
	mem_writeb(SegPhys(cs), 0x34);
	mem_writeb(SegPhys(cs) + 1, 0x12);
	run();
	EXPECT_EQ(reg_ax, 0x1234);
	EXPECT_EQ(reg_ip, 2);
}

TEST_F(Cpu8088Test, PushSpStoresDecrementedValue)
{
	code(std::array<uint8_t, 1>{0x54});
	run();
	EXPECT_EQ(reg_sp, 0xffe);
	EXPECT_EQ(CPU_Pop16(), 0xffe);
	code(std::array<uint8_t, 2>{0xff, 0xf4}); // Alternate PUSH SP encoding
	run();
	EXPECT_EQ(CPU_Pop16(), 0xffe);
}

TEST_F(Cpu8088Test, FlagsReservedBitsCannotBeChanged)
{
	CPU_SetFlags(0, FMASK_ALL);
	CPU_PUSHF(false);
	EXPECT_EQ(CPU_Pop16(), 0xf002);
	CPU_Push16(0xffff);
	CPU_POPF(false);
	CPU_PUSHF(false);
	EXPECT_EQ(CPU_Pop16(), 0xffd7);
}

TEST_F(Cpu8088Test, ClShiftCountIsNotMasked)
{
	code(std::array<uint8_t, 2>{0xd3, 0xe0}); // SHL AX,CL
	reg_ax = 1;
	reg_cl = 32;
	run();
	EXPECT_EQ(reg_ax, 0);
}

TEST_F(Cpu8088Test, ByteMemoryShiftUsesFullCount)
{
	CPU_SetSegGeneral(ds, 0x3000);
	code(std::array<uint8_t, 4>{0xd2, 0x26, 0x00, 0x02}); // SHL [0200h],CL
	mem_writeb(SegPhys(ds) + 0x200, 1);
	reg_cl = 32;
	run();
	EXPECT_EQ(mem_readb(SegPhys(ds) + 0x200), 0);
}

TEST_F(Cpu8088Test, ZeroShiftCountPreservesOperandAndFlags)
{
	code(std::array<uint8_t, 2>{0xd3, 0xe0});
	reg_ax = 0x1234;
	reg_cl = 0;
	CPU_SetFlags(FLAG_CF, FMASK_ALL);
	run();
	EXPECT_EQ(reg_ax, 0x1234);
	EXPECT_NE(reg_flags & FLAG_CF, 0u);
}

TEST_F(Cpu8088Test, LaterOperandPrefixIsAn8088ConditionalJump)
{
	code(std::array<uint8_t, 2>{0x66, 0x10}); // Undocumented JBE +10h
	CPU_SetFlags(FLAG_CF, FMASK_ALL);
	run();
	EXPECT_EQ(reg_ip, 0x112);
}

TEST_F(Cpu8088Test, ReturnAliasUses8088Encoding)
{
	code(std::array<uint8_t, 1>{0xc1}); // Undocumented RET
	CPU_Push16(0x4321);
	run();
	EXPECT_EQ(reg_ip, 0x4321);
	EXPECT_EQ(reg_sp, 0x1000);
}

TEST_F(Cpu8088Test, LaterCpuStillDecodesExtendedOpcode)
{
	configure("386");
	code(std::array<uint8_t, 3>{0x0f, 0xb6, 0xc3}); // MOVZX AX,BL
	reg_bl = 0x42;
	run();
	EXPECT_EQ(reg_ax, 0x42);
	EXPECT_EQ(reg_ip, 0x103);
	EXPECT_EQ(SegValue(cs), 0x1000);
	EXPECT_EQ(reg_sp, 0x1000);
}

TEST_F(Cpu8088Test, SwitchingCpuTypeRestoresReservedFlags)
{
	EXPECT_NE(reg_flags & 0x8000u, 0u);
	configure("386");
	EXPECT_EQ(reg_flags & 0x8000u, 0u);
	CPU_PUSHF(false);
	EXPECT_EQ(CPU_Pop16() & 0x8000, 0);
	configure("8088");
	EXPECT_NE(reg_flags & 0x8000u, 0u);
}

TEST_F(Cpu8088Test, LaterCpuKeepsMaskedShiftCount)
{
	configure("386");
	code(std::array<uint8_t, 2>{0xd3, 0xe0});
	reg_ax = 1;
	reg_cl = 32;
	run();
	EXPECT_EQ(reg_ax, 1);
}

TEST_F(Cpu8088Test, LaterCpuPushSpStoresOriginalValue)
{
	configure("386");
	code(std::array<uint8_t, 1>{0x54});
	run();
	EXPECT_EQ(CPU_Pop16(), 0x1000);
}

TEST_F(Cpu8088Test, AllCoreSelectionsUse8088Interpreter)
{
	for (const auto core : {"auto", "normal", "simple", "full", "dynamic"}) {
		SCOPED_TRACE(core);
		configure("8088", core);
		EXPECT_EQ(CPU_ArchitectureType, ArchitectureType::Intel86);
		EXPECT_EQ(cpudecoder, &CPU_Core_Normal_Run);
	}
}

} // namespace
