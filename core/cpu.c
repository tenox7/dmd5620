/*
 * WE32100 CPU. Port of cpu.rs.
 *
 * This is a faithful port: where the Rust core has quirks, this code
 * has the same quirks, so the two run the firmware the same way,
 * instruction for instruction (see test/difftest). Where the Rust core
 * panics, this code calls core_fatal().
 *
 * Copyright 2018-2025 Seth J. Morabito <web@loomcom.com> (Rust original)
 * Copyright 2026 the dmd5620 authors (C port)
 * MIT License, see LICENSE.md.
 */

#include <stdio.h>
#include <string.h>

#include "core.h"

/* PSW flags and offsets */
#define F_ET  0x00000003u
#define F_TM  0x00000004u
#define F_ISC 0x00000078u
#define F_I   0x00000080u
#define F_R   0x00000100u
#define F_PM  0x00000600u
#define F_CM  0x00001800u
#define F_IPL 0x0001e000u
#define F_C   0x00040000u
#define F_V   0x00080000u
#define F_Z   0x00100000u
#define F_N   0x00200000u
#define F_CD  0x00800000u
#define F_QIE 0x01000000u
#define F_CFD 0x02000000u

#define O_ET  0
#define O_TM  2
#define O_ISC 3

#define WE32100_VERSION 0x1a

static const uint32_t IPL_TABLE[64] = {
    0,  14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
};

/* Data types. D_NONE as an expanded type means "no expansion". */
enum { D_NONE, D_BYTE, D_HALF, D_WORD, D_SBYTE, D_UHALF, D_UWORD };

/* Addressing modes */
enum {
    M_NONE,
    M_ABSOLUTE,
    M_ABSOLUTE_DEFERRED,
    M_BYTE_DISP,
    M_BYTE_DISP_DEFERRED,
    M_HALF_DISP,
    M_HALF_DISP_DEFERRED,
    M_WORD_DISP,
    M_WORD_DISP_DEFERRED,
    M_AP_SHORT_OFFSET,
    M_FP_SHORT_OFFSET,
    M_BYTE_IMMEDIATE,
    M_HALF_IMMEDIATE,
    M_WORD_IMMEDIATE,
    M_POSITIVE_LITERAL,
    M_NEGATIVE_LITERAL,
    M_REGISTER,
    M_REGISTER_DEFERRED,
};

/* Operand types */
enum { OT_NONE, OT_LIT, OT_SRC, OT_DEST };

struct mnemonic {
    uint8_t valid;
    uint16_t opcode;
    uint8_t dtype;
    const char *name;
    uint8_t ops[4];
};

#include "optable.h"

struct operand {
    uint8_t size;
    uint8_t mode;
    uint8_t data_type;
    uint8_t expanded_type; /* D_NONE = none */
    int8_t reg;            /* -1 = none */
    uint32_t embedded;
    uint32_t data;
    uint32_t eff;
};

#define IR_DATA_LEN 32

static struct {
    uint16_t opcode;
    const char *name;
    uint8_t data_type;
    uint8_t len;
    uint8_t data[IR_DATA_LEN];
    struct operand operands[4];
} ir;

uint32_t cpu_r[16];
#define R cpu_r

/* Opcodes (instr.rs) */
enum {
    HALT = 0x00, MOVAW = 0x04, RET = 0x08, MOVTRW = 0x0C, SAVE = 0x10, RESTORE = 0x18,
    SWAPWI = 0x1C, SWAPHI = 0x1E, SWAPBI = 0x1F, POPW = 0x20, JMP = 0x24, CFLUSH = 0x27,
    TSTW = 0x28, TSTH = 0x2A, TSTB = 0x2B, CALL = 0x2C, BPT = 0x2E, JSB = 0x34,
    BSBH = 0x36, BSBB = 0x37, BITW = 0x38, BITH = 0x3A, BITB = 0x3B, CMPW = 0x3C,
    CMPH = 0x3E, CMPB = 0x3F, RGEQ = 0x40, BGEH = 0x42, BGEB = 0x43, RGTR = 0x44,
    BGH = 0x46, BGB = 0x47, RLSS = 0x48, BLH = 0x4A, BLB = 0x4B, RLEQ = 0x4C,
    BLEH = 0x4E, BLEB = 0x4F, RGEQU = 0x50, BGEUH = 0x52, BGEUB = 0x53, BGUH = 0x56,
    BGUB = 0x57, BLUH = 0x5A, BLUB = 0x5B, RLEQU = 0x5C, BLEUH = 0x5E, BLEUB = 0x5F,
    BVCH = 0x62, BVCB = 0x63, RNEQU = 0x64, BNEH_D = 0x66, BNEB_D = 0x67, BVSH = 0x6A,
    BVSB = 0x6B, REQLU = 0x6C, BEH_D = 0x6E, BEB_D = 0x6F, NOP = 0x70, NOP3 = 0x72,
    NOP2 = 0x73, RNEQ = 0x74, BNEH = 0x76, BNEB = 0x77, RSB = 0x78, BRH = 0x7A,
    BRB = 0x7B, REQL = 0x7C, BEH = 0x7E, BEB = 0x7F, CLRW = 0x80, CLRH = 0x82,
    CLRB = 0x83, MOVW = 0x84, MOVH = 0x86, MOVB = 0x87, MCOMW = 0x88, MCOMH = 0x8A,
    MCOMB = 0x8B, MNEGW = 0x8C, MNEGH = 0x8E, MNEGB = 0x8F, INCW = 0x90, INCH = 0x92,
    INCB = 0x93, DECW = 0x94, DECH = 0x96, DECB = 0x97, ADDW2 = 0x9C, ADDH2 = 0x9E,
    ADDB2 = 0x9F, PUSHW = 0xA0, MODW2 = 0xA4, MODH2 = 0xA6, MODB2 = 0xA7, MULW2 = 0xA8,
    MULH2 = 0xAA, MULB2 = 0xAB, DIVW2 = 0xAC, DIVH2 = 0xAE, DIVB2 = 0xAF, ORW2 = 0xB0,
    ORH2 = 0xB2, ORB2 = 0xB3, XORW2 = 0xB4, XORH2 = 0xB6, XORB2 = 0xB7, ANDW2 = 0xB8,
    ANDH2 = 0xBA, ANDB2 = 0xBB, SUBW2 = 0xBC, SUBH2 = 0xBE, SUBB2 = 0xBF, ALSW3 = 0xC0,
    ARSW3 = 0xC4, ARSH3 = 0xC6, ARSB3 = 0xC7, INSFW = 0xC8, INSFH = 0xCA, INSFB = 0xCB,
    EXTFW = 0xCC, EXTFH = 0xCE, EXTFB = 0xCF, LLSW3 = 0xD0, LLSH3 = 0xD2, LLSB3 = 0xD3,
    LRSW3 = 0xD4, ROTW = 0xD8, ADDW3 = 0xDC, ADDH3 = 0xDE, ADDB3 = 0xDF, PUSHAW = 0xE0,
    MODW3 = 0xE4, MODH3 = 0xE6, MODB3 = 0xE7, MULW3 = 0xE8, MULH3 = 0xEA, MULB3 = 0xEB,
    DIVW3 = 0xEC, DIVH3 = 0xEE, DIVB3 = 0xEF, ORW3 = 0xF0, ORH3 = 0xF2, ORB3 = 0xF3,
    XORW3 = 0xF4, XORH3 = 0xF6, XORB3 = 0xF7, ANDW3 = 0xF8, ANDH3 = 0xFA, ANDB3 = 0xFB,
    SUBW3 = 0xFC, SUBH3 = 0xFE, SUBB3 = 0xFF,
    MVERNO = 0x3009, ENBVJMP = 0x300D, DISVJMP = 0x3013, MOVBLW = 0x3019,
    STREND = 0x301F, RETG = 0x3045, CALLPS = 0x30AC, RETPS = 0x30C8,
};

/* ---- Helpers ---- */

static uint32_t sign_extend_halfword(uint16_t v)
{
    return (uint32_t)(int32_t)(int16_t)v;
}

static uint32_t sign_extend_byte(uint8_t v)
{
    return (uint32_t)(int32_t)(int8_t)v;
}

static uint32_t add_offset(uint32_t val, uint32_t offset)
{
    return val + offset; /* wrapping */
}

static uint8_t op_data_type(const struct operand *op)
{
    return op->expanded_type != D_NONE ? op->expanded_type : op->data_type;
}

static void set_c_flag(bool set)
{
    if (set)
        R[R_PSW] |= F_C;
    else
        R[R_PSW] &= ~F_C;
}

static void set_v_flag(bool set)
{
    if (set)
        R[R_PSW] |= F_V;
    else
        R[R_PSW] &= ~F_V;
}

static void set_z_flag(bool set)
{
    if (set)
        R[R_PSW] |= F_Z;
    else
        R[R_PSW] &= ~F_Z;
}

static void set_n_flag(bool set)
{
    if (set)
        R[R_PSW] |= F_N;
    else
        R[R_PSW] &= ~F_N;
}

static bool c_flag(void) { return (R[R_PSW] & F_C) != 0; }
static bool v_flag(void) { return (R[R_PSW] & F_V) != 0; }
static bool z_flag(void) { return (R[R_PSW] & F_Z) != 0; }
static bool n_flag(void) { return (R[R_PSW] & F_N) != 0; }

static void set_isc(uint32_t val)
{
    R[R_PSW] &= ~F_ISC;
    R[R_PSW] |= (val & 0xf) << 3;
}

static bool is_kernel(void)
{
    return ((R[R_PSW] & F_CM) >> 11 & 3) == 0;
}

/* V flag from the operand's (not expanded) data type */
static void set_v_flag_op(uint32_t val, int index)
{
    switch (ir.operands[index].data_type) {
    case D_WORD:
    case D_UWORD:
        set_v_flag(false);
        break;
    case D_HALF:
    case D_UHALF:
        set_v_flag(val > 0xffff);
        break;
    case D_BYTE:
    case D_SBYTE:
        set_v_flag(val > 0xff);
        break;
    default:
        break;
    }
}

static void set_nz_flags(uint32_t val, int index)
{
    switch (ir.operands[index].data_type) {
    case D_WORD:
    case D_UWORD:
        set_n_flag((val & 0x80000000u) != 0);
        set_z_flag(val == 0);
        break;
    case D_HALF:
    case D_UHALF:
        set_n_flag((val & 0x8000) != 0);
        set_z_flag((uint16_t)val == 0);
        break;
    case D_BYTE:
    case D_SBYTE:
        set_n_flag((val & 0x80) != 0);
        set_z_flag((uint8_t)val == 0);
        break;
    default:
        break;
    }
}

static int stack_push(uint32_t val)
{
    TRY(bus_write_word(R[R_SP], val));
    R[R_SP] += 4;
    return E_OK;
}

static int stack_pop(uint32_t *out)
{
    TRY(bus_read_word((uint32_t)(R[R_SP] - 4), out));
    R[R_SP] -= 4;
    return E_OK;
}

static int irq_push(uint32_t val)
{
    TRY(bus_write_word(R[R_ISP], val));
    R[R_ISP] += 4;
    return E_OK;
}

static int irq_pop(uint32_t *out)
{
    R[R_ISP] -= 4;
    return bus_read_word(R[R_ISP], out);
}

/* ---- Reset ---- */

void cpu_new(void)
{
    memset(R, 0, sizeof R);
    memset(&ir, 0, sizeof ir);
    ir.name = "???";
    for (int i = 0; i < 4; i++)
        ir.operands[i].reg = -1;
}

/* WE32100 manual page 2-52: fetch PCBP from 0x80, then PSW, PC and SP
 * from the PCB. */
int cpu_reset(void)
{
    TRY(bus_read_word(0x80, &R[R_PCBP]));
    TRY(bus_read_word(R[R_PCBP], &R[R_PSW]));
    TRY(bus_read_word((addr_t)R[R_PCBP] + 4, &R[R_PC]));
    TRY(bus_read_word((addr_t)R[R_PCBP] + 8, &R[R_SP]));
    if (R[R_PSW] & F_I) {
        R[R_PSW] &= ~F_I;
        R[R_PCBP] += 12;
    }
    set_isc(3);
    return E_OK;
}

/* ---- Operands ---- */

static int effective_address(int index, uint32_t *out)
{
    struct operand *op = &ir.operands[index];
    uint32_t embedded = op->embedded;
    int r = op->reg;
    uint32_t addr;

    switch (op->mode) {
    case M_REGISTER_DEFERRED:
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        addr = R[r];
        break;
    case M_ABSOLUTE:
        addr = embedded;
        break;
    case M_ABSOLUTE_DEFERRED:
        TRY(bus_read_word(embedded, &addr));
        break;
    case M_FP_SHORT_OFFSET:
        addr = add_offset(R[R_FP], sign_extend_byte((uint8_t)embedded));
        break;
    case M_AP_SHORT_OFFSET:
        addr = add_offset(R[R_AP], sign_extend_byte((uint8_t)embedded));
        break;
    case M_WORD_DISP:
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        addr = add_offset(R[r], embedded);
        break;
    case M_WORD_DISP_DEFERRED:
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        TRY(bus_read_word(add_offset(R[r], embedded), &addr));
        break;
    case M_HALF_DISP:
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        addr = add_offset(R[r], sign_extend_halfword((uint16_t)embedded));
        break;
    case M_HALF_DISP_DEFERRED:
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        TRY(bus_read_word(add_offset(R[r], sign_extend_halfword((uint16_t)embedded)), &addr));
        break;
    case M_BYTE_DISP:
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        addr = add_offset(R[r], sign_extend_byte((uint8_t)embedded));
        break;
    case M_BYTE_DISP_DEFERRED:
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        TRY(bus_read_word(add_offset(R[r], sign_extend_byte((uint8_t)embedded)), &addr));
        break;
    default:
        return E_ILLEGAL_OPCODE;
    }

    op->data = addr;
    *out = addr;
    return E_OK;
}

static int read_op(int index, uint32_t *out)
{
    const struct operand *op = &ir.operands[index];
    uint32_t val, eff;
    uint16_t h;
    uint8_t b;

    switch (op->mode) {
    case M_REGISTER:
        if (op->reg < 0)
            return E_ILLEGAL_OPCODE;
        switch (op_data_type(op)) {
        case D_WORD:
        case D_UWORD:
            val = R[op->reg];
            break;
        case D_HALF:
            val = sign_extend_halfword((uint16_t)R[op->reg]);
            break;
        case D_UHALF:
            val = (uint16_t)R[op->reg];
            break;
        case D_BYTE:
            val = (uint8_t)R[op->reg];
            break;
        case D_SBYTE:
            val = sign_extend_byte((uint8_t)R[op->reg]);
            break;
        default:
            return E_ILLEGAL_OPCODE;
        }
        break;
    case M_POSITIVE_LITERAL:
    case M_NEGATIVE_LITERAL:
        val = sign_extend_byte((uint8_t)op->embedded);
        break;
    case M_WORD_IMMEDIATE:
        val = op->embedded;
        break;
    case M_HALF_IMMEDIATE:
        val = sign_extend_halfword((uint16_t)op->embedded);
        break;
    case M_BYTE_IMMEDIATE:
        val = sign_extend_byte((uint8_t)op->embedded);
        break;
    default:
        TRY(effective_address(index, &eff));
        switch (op_data_type(op)) {
        case D_UWORD:
        case D_WORD:
            TRY(bus_read_word(eff, &val));
            break;
        case D_HALF:
            TRY(bus_read_half(eff, &h));
            val = sign_extend_halfword(h);
            break;
        case D_UHALF:
            TRY(bus_read_half(eff, &h));
            val = h;
            break;
        case D_BYTE:
            TRY(bus_read_byte(eff, &b));
            val = b;
            break;
        case D_SBYTE:
            TRY(bus_read_byte(eff, &b));
            val = sign_extend_byte(b);
            break;
        default:
            return E_ILLEGAL_OPCODE;
        }
        break;
    }

    *out = val;
    return E_OK;
}

static int write_op(int index, uint32_t val)
{
    struct operand *op = &ir.operands[index];
    uint8_t dt = op_data_type(op);
    uint32_t eff;

    op->data = val;

    switch (op->mode) {
    case M_REGISTER:
        if (op->reg < 0)
            return E_ILLEGAL_OPCODE;
        R[op->reg] = val;
        break;
    case M_NEGATIVE_LITERAL:
    case M_POSITIVE_LITERAL:
    case M_BYTE_IMMEDIATE:
    case M_HALF_IMMEDIATE:
    case M_WORD_IMMEDIATE:
        return E_ILLEGAL_OPCODE;
    default:
        TRY(effective_address(index, &eff));
        op->eff = eff;
        switch (dt) {
        case D_UWORD:
        case D_WORD:
            TRY(bus_write_word(eff, val));
            break;
        case D_HALF:
        case D_UHALF:
            TRY(bus_write_half(eff, (uint16_t)val));
            break;
        case D_BYTE:
        case D_SBYTE:
            TRY(bus_write_byte(eff, (uint8_t)val));
            break;
        default:
            return E_ILLEGAL_OPCODE;
        }
        break;
    }
    return E_OK;
}

/* ---- Context switches ---- */

static int context_switch_1(uint32_t new_pcbp)
{
    uint32_t w;

    /* Save the current PC in the PCB */
    TRY(bus_write_word((uint32_t)(R[R_PCBP] + 4), R[R_PC]));

    /* Copy the 'R' flag from the new PSW to the old PSW */
    R[R_PSW] &= ~F_R;
    TRY(bus_read_word(new_pcbp, &w));
    R[R_PSW] |= w & F_R;

    /* Save the current PSW and SP in the old PCB */
    TRY(bus_write_word(R[R_PCBP], R[R_PSW]));
    TRY(bus_write_word((uint32_t)(R[R_PCBP] + 8), R[R_SP]));

    /* If R is set, save the current R0-R8, FP, AP in the PCB */
    if (R[R_PSW] & F_R) {
        TRY(bus_write_word((uint32_t)(R[R_PCBP] + 24), R[R_FP]));
        for (int i = 0; i <= 8; i++)
            TRY(bus_write_word((uint32_t)(R[R_PCBP] + 28 + 4 * i), R[i]));
        TRY(bus_write_word((uint32_t)(R[R_PCBP] + 20), R[R_AP]));
        R[R_FP] = R[R_PCBP] + 52;
    }
    return E_OK;
}

static int context_switch_2(uint32_t new_pcbp)
{
    R[R_PCBP] = new_pcbp;

    /* Put new PSW, PC and SP values from the PCB into registers */
    TRY(bus_read_word(R[R_PCBP], &R[R_PSW]));
    R[R_PSW] &= ~F_TM;
    TRY(bus_read_word((uint32_t)(R[R_PCBP] + 4), &R[R_PC]));
    TRY(bus_read_word((uint32_t)(R[R_PCBP] + 8), &R[R_SP]));

    /* If the I bit is set, move the PCBP past the initial context area */
    if (R[R_PSW] & F_I) {
        R[R_PSW] &= ~F_I;
        R[R_PCBP] += 12;
    }
    return E_OK;
}

static int context_switch_3(void)
{
    uint32_t tmp;

    if (R[R_PSW] & F_R) {
        R[0] = R[R_PCBP] + 64;
        TRY(bus_read_word(R[0], &R[2]));
        R[0] += 4;

        while (R[2] != 0) {
            TRY(bus_read_word(R[0], &R[1]));
            R[0] += 4;

            /* A MOVBLW inside this loop */
            while (R[2] != 0) {
                TRY(bus_read_word(R[0], &tmp));
                TRY(bus_write_word(R[1], tmp));
                R[2] -= 1;
                R[0] += 4;
                R[1] += 4;
            }

            TRY(bus_read_word(R[0], &R[2]));
            R[0] += 4;
        }

        R[0] += 4;
    }
    return E_OK;
}

/* ---- Arithmetic ---- */

static int add(uint32_t a, uint32_t b, int dst)
{
    uint64_t result = (uint64_t)a + (uint64_t)b;
    uint32_t r32 = (uint32_t)result;

    TRY(write_op(dst, r32));
    set_nz_flags(r32, dst);

    switch (op_data_type(&ir.operands[dst])) {
    case D_WORD:
    case D_UWORD:
        set_c_flag(result > 0xffffffffu);
        set_v_flag((((a ^ ~b) & (a ^ r32)) & 0x80000000u) != 0);
        break;
    case D_HALF:
    case D_UHALF:
        set_c_flag(result > 0xffff);
        set_v_flag((((a ^ ~b) & (a ^ r32)) & 0x8000) != 0);
        break;
    case D_BYTE:
    case D_SBYTE:
        set_c_flag(result > 0xff);
        set_v_flag((((a ^ ~b) & (a ^ r32)) & 0x80) != 0);
        break;
    default:
        return E_ILLEGAL_OPCODE;
    }
    return E_OK;
}

static int sub(uint32_t a, uint32_t b, int dst)
{
    uint32_t r32 = (uint32_t)((uint64_t)a - (uint64_t)b);

    TRY(write_op(dst, r32));
    set_nz_flags(r32, dst);
    set_c_flag(b > a);
    set_v_flag_op(r32, dst);
    return E_OK;
}

/* b / a and b % a, by the (not expanded) data type of operand dst.
 * The Rust core panics on a zero divisor in the narrowed type and on
 * signed overflow (MIN / -1). Here a zero divisor is fatal too, and
 * signed overflow wraps. */
static uint32_t divmod(uint32_t a, uint32_t b, int dst, bool mod)
{
    switch (ir.operands[dst].data_type) {
    case D_WORD: {
        int32_t x = (int32_t)b, y = (int32_t)a;
        if (y == 0)
            core_fatal("attempt to divide by zero");
        if (x == INT32_MIN && y == -1)
            return mod ? 0 : (uint32_t)x;
        return (uint32_t)(mod ? x % y : x / y);
    }
    case D_HALF: {
        int16_t x = (int16_t)b, y = (int16_t)a;
        if (y == 0)
            core_fatal("attempt to divide by zero");
        if (x == INT16_MIN && y == -1)
            return mod ? 0 : (uint32_t)(int32_t)x;
        return (uint32_t)(int32_t)(int16_t)(mod ? x % y : x / y);
    }
    case D_SBYTE: {
        int8_t x = (int8_t)b, y = (int8_t)a;
        if (y == 0)
            core_fatal("attempt to divide by zero");
        if (x == INT8_MIN && y == -1)
            return mod ? 0 : (uint32_t)(int32_t)x;
        return (uint32_t)(int32_t)(int8_t)(mod ? x % y : x / y);
    }
    case D_UHALF: {
        uint16_t x = (uint16_t)b, y = (uint16_t)a;
        if (y == 0)
            core_fatal("attempt to divide by zero");
        return (uint16_t)(mod ? x % y : x / y);
    }
    case D_BYTE: {
        uint8_t x = (uint8_t)b, y = (uint8_t)a;
        if (y == 0)
            core_fatal("attempt to divide by zero");
        return (uint8_t)(mod ? x % y : x / y);
    }
    default: /* D_UWORD, D_NONE */
        if (a == 0)
            core_fatal("attempt to divide by zero");
        return mod ? b % a : b / a;
    }
}

/* ---- Interrupts and exceptions ---- */

static void on_interrupt(uint8_t vector)
{
    uint32_t new_pcbp;
    int e;

    if ((e = bus_read_word(0x8c + 4 * (uint32_t)vector, &new_pcbp)) ||
        (e = irq_push(R[R_PCBP])))
        goto fail;

    R[R_PSW] &= ~(F_ISC | F_TM | F_ET);
    R[R_PSW] |= 1;

    if ((e = context_switch_1(new_pcbp)) || (e = context_switch_2(new_pcbp)))
        goto fail;

    R[R_PSW] &= ~(F_ISC | F_TM | F_ET);
    R[R_PSW] |= 7 << 3;
    R[R_PSW] |= 3;

    if ((e = context_switch_3()))
        goto fail;
    return;
fail:
    core_fatal("interrupt %02x failed: %s, PC=%08x", vector, core_error_name(e), R[R_PC]);
}

static int gate(addr_t index1, addr_t index2)
{
    uint32_t w, new_psw;

    TRY(bus_read_word(index1, &w));
    addr_t gate_l2 = (addr_t)w + index2;

    TRY(bus_read_word(gate_l2, &new_psw));

    /* Clear state in the new PSW */
    new_psw &= ~(F_PM | F_IPL | F_R | F_ISC | F_TM | F_ET);

    /* Set new state */
    new_psw |= (R[R_PSW] & F_CM) >> 2; /* PM from CM */
    new_psw |= R[R_PSW] & F_IPL;
    new_psw |= R[R_PSW] & F_R;

    /* New ISC, TM and ET: 7, 1, 3 */
    new_psw |= 7 << O_ISC;
    new_psw |= 1 << O_TM;
    new_psw |= 3 << O_ET;

    /* Then the new PC and PSW */
    TRY(bus_read_word(gate_l2 + 4, &R[R_PC]));
    R[R_PSW] = new_psw;
    return E_OK;
}

/* The only exception the Rust core handles: external memory fault */
static int on_exception_external_memory(void)
{
    const uint32_t et = 3, isc = 5;

    R[R_PSW] &= ~F_ET;
    R[R_PSW] &= ~F_ISC;
    R[R_PSW] |= et;
    R[R_PSW] |= isc << O_ISC;

    /* Push the address of the faulting instruction */
    TRY(bus_write_word(R[R_SP], R[R_PC]));

    /* Write 0, 3 to the PSW ET and ISC fields */
    R[R_PSW] &= ~(F_ET | F_ISC);
    R[R_PSW] |= 3 << O_ISC;

    /* Push the PSW */
    TRY(bus_write_word((addr_t)R[R_SP] + 4, R[R_PSW]));

    TRY(gate(0, (addr_t)isc << O_ISC));

    /* Finish the stack push */
    R[R_SP] += 8;
    return E_OK;
}

/* ---- Decoder ---- */

static void set_operand(int index, uint8_t size, uint8_t mode, uint8_t data_type,
                        uint8_t expanded_type, int reg, uint32_t embedded)
{
    struct operand *op = &ir.operands[index];
    op->size = size;
    op->mode = mode;
    op->data_type = data_type;
    op->expanded_type = expanded_type;
    op->reg = (int8_t)reg;
    op->embedded = embedded;
}

static void ir_need(int n)
{
    if (ir.len + n > IR_DATA_LEN)
        core_fatal("instruction too long at PC=%08x", R[R_PC]);
}

static int acc_byte(uint8_t *out)
{
    uint8_t b;
    TRY(bus_read_byte((addr_t)R[R_PC] + ir.len, &b));
    ir_need(1);
    ir.data[ir.len++] = b;
    *out = b;
    return E_OK;
}

static int acc_half(uint16_t *out)
{
    uint16_t h;
    TRY(bus_read_op_half((addr_t)R[R_PC] + ir.len, &h));
    ir_need(2);
    ir.data[ir.len] = (uint8_t)h;
    ir.data[ir.len + 1] = (uint8_t)(h >> 8);
    ir.len += 2;
    *out = h;
    return E_OK;
}

static int acc_word(uint32_t *out)
{
    uint32_t w;
    TRY(bus_read_op_word((addr_t)R[R_PC] + ir.len, &w));
    ir_need(4);
    for (int i = 0; i < 4; i++)
        ir.data[ir.len + i] = (uint8_t)(w >> (8 * i));
    ir.len += 4;
    *out = w;
    return E_OK;
}

/* A literal operand: data with no descriptor byte right after the opcode */
static int decode_literal_operand(int index, const struct mnemonic *mn)
{
    uint8_t b;
    uint16_t h;
    uint32_t w;

    switch (mn->dtype) {
    case D_BYTE:
        TRY(acc_byte(&b));
        set_operand(index, 1, M_NONE, D_BYTE, D_NONE, -1, b);
        break;
    case D_HALF:
        TRY(acc_half(&h));
        set_operand(index, 2, M_NONE, D_HALF, D_NONE, -1, h);
        break;
    case D_WORD:
        TRY(acc_word(&w));
        set_operand(index, 4, M_NONE, D_WORD, D_NONE, -1, w);
        break;
    default:
        return E_ILLEGAL_OPCODE;
    }
    return E_OK;
}

static int decode_descriptor_operand(int index, uint8_t dtype, uint8_t etype, bool recur)
{
    uint8_t db, b;
    uint16_t h;
    uint32_t w;

    TRY(acc_byte(&db));

    uint8_t m = (db & 0xf0) >> 4;
    uint8_t r = db & 0xf;
    uint8_t dsize = recur ? 2 : 1;

    switch (m) {
    case 0:
    case 1:
    case 2:
    case 3: /* Positive literal */
        set_operand(index, dsize, M_POSITIVE_LITERAL, dtype, etype, -1, db);
        break;
    case 4:
        if (r == 15) { /* Word immediate */
            TRY(acc_word(&w));
            set_operand(index, dsize + 4, M_WORD_IMMEDIATE, dtype, etype, -1, w);
        } else { /* Register */
            set_operand(index, dsize, M_REGISTER, dtype, etype, r, 0);
        }
        break;
    case 5:
        if (r == 15) { /* Halfword immediate */
            TRY(acc_half(&h));
            set_operand(index, dsize + 2, M_HALF_IMMEDIATE, dtype, etype, -1, h);
        } else if (r == 11) {
            return E_ILLEGAL_OPCODE;
        } else { /* Register deferred */
            set_operand(index, dsize, M_REGISTER_DEFERRED, dtype, etype, r, 0);
        }
        break;
    case 6:
        if (r == 15) { /* Byte immediate */
            TRY(acc_byte(&b));
            set_operand(index, dsize + 1, M_BYTE_IMMEDIATE, dtype, etype, -1, b);
        } else { /* FP short offset */
            set_operand(index, dsize, M_FP_SHORT_OFFSET, dtype, etype, R_FP, r);
        }
        break;
    case 7:
        if (r == 15) { /* Absolute */
            TRY(acc_word(&w));
            set_operand(index, dsize + 4, M_ABSOLUTE, dtype, etype, -1, w);
        } else { /* AP short offset */
            set_operand(index, dsize, M_AP_SHORT_OFFSET, dtype, etype, R_AP, r);
        }
        break;
    case 8:
    case 9:
        if (r == 11)
            return E_ILLEGAL_OPCODE;
        TRY(acc_word(&w));
        set_operand(index, dsize + 4, m == 8 ? M_WORD_DISP : M_WORD_DISP_DEFERRED, dtype,
                    etype, r, w);
        break;
    case 10:
    case 11:
        if (r == 11)
            return E_ILLEGAL_OPCODE;
        TRY(acc_half(&h));
        set_operand(index, dsize + 2, m == 10 ? M_HALF_DISP : M_HALF_DISP_DEFERRED, dtype,
                    etype, r, h);
        break;
    case 12:
    case 13:
        if (r == 11)
            return E_ILLEGAL_OPCODE;
        TRY(acc_byte(&b));
        set_operand(index, dsize + 1, m == 12 ? M_BYTE_DISP : M_BYTE_DISP_DEFERRED, dtype,
                    etype, r, b);
        break;
    case 14:
        switch (r) {
        case 0:
            return decode_descriptor_operand(index, dtype, D_UWORD, true);
        case 2:
            return decode_descriptor_operand(index, dtype, D_UHALF, true);
        case 3:
            return decode_descriptor_operand(index, dtype, D_BYTE, true);
        case 4:
            return decode_descriptor_operand(index, dtype, D_WORD, true);
        case 6:
            return decode_descriptor_operand(index, dtype, D_HALF, true);
        case 7:
            return decode_descriptor_operand(index, dtype, D_SBYTE, true);
        case 15: /* Absolute deferred */
            TRY(acc_word(&w));
            set_operand(index, dsize + 4, M_ABSOLUTE_DEFERRED, dtype, etype, -1, w);
            break;
        default:
            return E_ILLEGAL_OPCODE;
        }
        break;
    case 15: /* Negative literal */
        set_operand(index, 1, M_NEGATIVE_LITERAL, dtype, etype, -1, db);
        break;
    }
    return E_OK;
}

static int decode_instruction(void)
{
    const struct mnemonic *mn = NULL;
    uint8_t b1, b2;

    ir.len = 0;
    TRY(acc_byte(&b1));

    if (b1 == 0x30) {
        TRY(acc_byte(&b2));
        uint16_t opcode = (uint16_t)((b1 << 8) | b2);
        for (size_t i = 0; i < sizeof halfword_mnemonics / sizeof halfword_mnemonics[0]; i++) {
            if (halfword_mnemonics[i].opcode == opcode) {
                mn = &halfword_mnemonics[i];
                break;
            }
        }
    } else if (byte_mnemonics[b1].valid) {
        mn = &byte_mnemonics[b1];
    }

    if (!mn)
        return E_ILLEGAL_OPCODE;

    uint8_t etype = D_NONE;
    for (int index = 0; index < 4; index++) {
        struct operand *op = &ir.operands[index];
        switch (mn->ops[index]) {
        case OT_NONE:
            op->size = 0;
            op->mode = M_NONE;
            op->data_type = D_NONE;
            op->expanded_type = D_NONE;
            op->reg = -1;
            op->embedded = 0;
            continue;
        case OT_LIT:
            TRY(decode_literal_operand(index, mn));
            break;
        default:
            TRY(decode_descriptor_operand(index, mn->dtype, etype, false));
            break;
        }
        etype = op->expanded_type;
    }

    ir.opcode = mn->opcode;
    ir.name = mn->name;
    ir.data_type = mn->dtype;
    return E_OK;
}

/* ---- Execution ---- */

#define BRANCH_H() ((int32_t)sign_extend_halfword((uint16_t)ir.operands[0].embedded))
#define BRANCH_B() ((int32_t)sign_extend_byte((uint8_t)ir.operands[0].embedded))
#define READ(i, v) TRY(read_op((i), &(v)))

static int dispatch(int32_t *pc_inc)
{
    uint32_t a, b, c, result, val;
    int32_t pc_increment;

    /* Update anything that needs updating */
    core_tick();
    duart_service();

    uint8_t ivec = duart_get_interrupt();
    if (ivec != 0) {
        uint32_t cpu_ipl = (R[R_PSW] >> 13) & 0xf;
        if (cpu_ipl < IPL_TABLE[ivec & 0x3f])
            on_interrupt((uint8_t)(~ivec & 0x3f));
    }

    TRY(decode_instruction());
    pc_increment = ir.len;

    switch (ir.opcode) {
    case NOP:
        pc_increment = 1;
        break;
    case NOP2:
        pc_increment = 2;
        break;
    case NOP3:
        pc_increment = 3;
        break;
    case ADDW2:
    case ADDH2:
    case ADDB2:
        READ(0, a);
        READ(1, b);
        TRY(add(a, b, 1));
        break;
    case ADDW3:
    case ADDH3:
    case ADDB3:
        READ(0, a);
        READ(1, b);
        TRY(add(a, b, 2));
        break;
    case ALSW3:
        READ(0, a);
        READ(1, b);
        result = (uint32_t)((uint64_t)b << (a & 0x1f));
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag_op(result, 2);
        break;
    case ANDW2:
    case ANDH2:
    case ANDB2:
        READ(0, a);
        READ(1, b);
        result = a & b;
        TRY(write_op(1, result));
        set_nz_flags(result, 1);
        set_c_flag(false);
        set_v_flag_op(result, 1);
        break;
    case ANDW3:
    case ANDH3:
    case ANDB3:
        READ(0, a);
        READ(1, b);
        result = a & b;
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag_op(result, 2);
        break;
    case BEH:
    case BEH_D:
        if (z_flag())
            pc_increment = BRANCH_H();
        break;
    case BEB:
    case BEB_D:
        if (z_flag())
            pc_increment = BRANCH_B();
        break;
    case BGH:
        if (!(n_flag() || z_flag()))
            pc_increment = BRANCH_H();
        break;
    case BGB:
        if (!(n_flag() || z_flag()))
            pc_increment = BRANCH_B();
        break;
    case BGEH:
        if (!n_flag() || z_flag())
            pc_increment = BRANCH_H();
        break;
    case BGEB:
        if (!n_flag() || z_flag())
            pc_increment = BRANCH_B();
        break;
    case BGEUH:
        if (!c_flag())
            pc_increment = BRANCH_H();
        break;
    case BGEUB:
        if (!c_flag())
            pc_increment = BRANCH_B();
        break;
    case BGUH:
        if (!(c_flag() || z_flag()))
            pc_increment = BRANCH_H();
        break;
    case BGUB:
        if (!(c_flag() || z_flag()))
            pc_increment = BRANCH_B();
        break;
    case BITW:
    case BITH:
    case BITB:
        READ(0, a);
        READ(1, b);
        result = a & b;
        set_nz_flags(result, 1);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case BLH:
        if (n_flag() && !z_flag())
            pc_increment = BRANCH_H();
        break;
    case BLB:
        if (n_flag() && !z_flag())
            pc_increment = BRANCH_B();
        break;
    case BLEH:
        if (n_flag() || z_flag())
            pc_increment = BRANCH_H();
        break;
    case BLEB:
        if (n_flag() || z_flag())
            pc_increment = BRANCH_B();
        break;
    case BLEUH:
        if (c_flag() || z_flag())
            pc_increment = BRANCH_H();
        break;
    case BLEUB:
        if (c_flag() || z_flag())
            pc_increment = BRANCH_B();
        break;
    case BLUH:
        if (c_flag())
            pc_increment = BRANCH_H();
        break;
    case BLUB:
        if (c_flag())
            pc_increment = BRANCH_B();
        break;
    case BNEH:
    case BNEH_D:
        if (!z_flag())
            pc_increment = BRANCH_H();
        break;
    case BNEB:
    case BNEB_D:
        if (!z_flag())
            pc_increment = BRANCH_B();
        break;
    case BPT:
    case HALT:
        core_fatal("not implemented: %s at PC=%08x", ir.name, R[R_PC]);
    case BRH:
        pc_increment = BRANCH_H();
        break;
    case BRB:
        pc_increment = BRANCH_B();
        break;
    case BSBH:
    case BSBB: {
        int32_t offset = ir.opcode == BSBH ? BRANCH_H() : BRANCH_B();
        uint32_t return_pc = R[R_PC] + (uint32_t)pc_increment;
        TRY(stack_push(return_pc));
        pc_increment = offset;
        break;
    }
    case BVCH:
        if (!v_flag())
            pc_increment = BRANCH_H();
        break;
    case BVCB:
        if (!v_flag())
            pc_increment = BRANCH_B();
        break;
    case BVSH:
        if (v_flag())
            pc_increment = BRANCH_H();
        break;
    case BVSB:
        if (v_flag())
            pc_increment = BRANCH_B();
        break;
    case CALL: {
        TRY(effective_address(0, &a));
        TRY(effective_address(1, &b));
        uint32_t return_pc = R[R_PC] + (uint32_t)pc_increment;
        TRY(bus_write_word((uint32_t)(R[R_SP] + 4), R[R_AP]));
        TRY(bus_write_word(R[R_SP], return_pc));
        R[R_SP] += 8;
        R[R_PC] = b;
        R[R_AP] = a;
        pc_increment = 0;
        break;
    }
    case CFLUSH:
        break;
    case CALLPS:
        if (!is_kernel())
            return E_PRIVILEGED_OPCODE;
        a = R[0];
        TRY(irq_push(R[R_PCBP]));
        /* The PC goes to the next instruction (always PC + 2) */
        pc_increment = 0;
        R[R_PC] += 2;
        /* Old PSW ISC, TM, ET = 0, 0, 1 */
        R[R_PSW] &= ~(F_ISC | F_TM | F_ET);
        R[R_PSW] |= 1 << O_ET;
        TRY(context_switch_1(a));
        TRY(context_switch_2(a));
        R[R_PSW] &= ~(F_ISC | F_TM | F_ET);
        R[R_PSW] |= 7 << O_ISC;
        R[R_PSW] |= 3 << O_ET;
        TRY(context_switch_3());
        break;
    case CLRW:
    case CLRH:
    case CLRB:
        TRY(write_op(0, 0));
        set_n_flag(false);
        set_z_flag(true);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case CMPW:
        READ(0, a);
        READ(1, b);
        set_z_flag(b == a);
        set_n_flag((int32_t)b < (int32_t)a);
        set_c_flag(b < a);
        set_v_flag(false);
        break;
    case CMPH:
        READ(0, a);
        READ(1, b);
        set_z_flag((uint16_t)b == (uint16_t)a);
        set_n_flag((int16_t)b < (int16_t)a);
        set_c_flag((uint16_t)b < (uint16_t)a);
        set_v_flag(false);
        break;
    case CMPB:
        READ(0, a);
        READ(1, b);
        set_z_flag((uint8_t)b == (uint8_t)a);
        set_n_flag((int8_t)b < (int8_t)a);
        set_c_flag((uint8_t)b < (uint8_t)a);
        set_v_flag(false);
        break;
    case DECW:
    case DECH:
    case DECB:
        READ(0, a);
        TRY(sub(a, 1, 0));
        break;
    case DIVW2:
    case DIVH2:
    case DIVB2:
    case DIVW3:
    case DIVH3:
    case DIVB3: {
        int dst = (ir.opcode == DIVW2 || ir.opcode == DIVH2 || ir.opcode == DIVB2) ? 1 : 2;
        READ(0, a);
        READ(1, b);
        if (a == 0)
            return E_ZERO_DIVIDE;
        if ((ir.opcode == DIVW2 || ir.opcode == DIVW3) && a == 0xffffffffu && b == 0x80000000u)
            set_v_flag(true);
        if ((ir.opcode == DIVH2 || ir.opcode == DIVH3) && a == 0xffff && b == 0x8000)
            set_v_flag(true);
        if ((ir.opcode == DIVB2 || ir.opcode == DIVB3) && a == 0xff && b == 0x80)
            set_v_flag(true);
        result = divmod(a, b, 1, false);
        TRY(write_op(dst, result));
        set_nz_flags(result, dst);
        set_c_flag(false);
        break;
    }
    case MVERNO:
        R[0] = WE32100_VERSION;
        break;
    case ENBVJMP:
    case DISVJMP:
        if (!is_kernel())
            return E_PRIVILEGED_OPCODE;
        /* TODO (Rust core): enable/disable the MMU, if present */
        R[R_PC] = R[0];
        pc_increment = 0;
        break;
    case EXTFW:
    case EXTFH:
    case EXTFB: {
        READ(0, a);
        uint32_t width = (a & 0x1f) + 1;
        READ(1, b);
        uint32_t offset = b & 0x1f;
        uint32_t mask = width >= 32 ? 0xffffffffu : (1u << width) - 1;
        mask <<= offset;
        if (width + offset > 32)
            mask |= (1u << ((width + offset) - 32)) - 1;
        READ(2, a);
        a &= mask;
        a >>= offset;
        TRY(write_op(3, a));
        set_nz_flags(a, 3);
        set_c_flag(false);
        set_v_flag_op(a, 3);
        break;
    }
    case INCW:
    case INCH:
    case INCB:
        READ(0, a);
        TRY(add(a, 1, 0));
        break;
    case INSFW:
    case INSFH:
    case INSFB: {
        READ(0, a);
        uint32_t width = (a & 0x1f) + 1;
        READ(1, b);
        uint32_t offset = b & 0x1f;
        uint32_t mask = width >= 32 ? 0xffffffffu : (1u << width) - 1;
        READ(2, a);
        a &= mask;
        READ(3, b);
        b &= ~(mask << offset);
        b |= a << offset;
        TRY(write_op(3, b));
        set_nz_flags(b, 3);
        set_c_flag(false);
        set_v_flag_op(b, 3);
        break;
    }
    case JMP:
        TRY(effective_address(0, &R[R_PC]));
        pc_increment = 0;
        break;
    case JSB:
        TRY(stack_push(R[R_PC] + (uint32_t)pc_increment));
        TRY(effective_address(0, &R[R_PC]));
        pc_increment = 0;
        break;
    case LLSW3:
    case LLSH3:
    case LLSB3:
        READ(1, a);
        READ(0, b);
        b &= 0x1f;
        result = a << b;
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag_op(result, 2);
        break;
    case ARSW3:
    case ARSH3:
    case ARSB3:
        READ(1, a);
        READ(0, b);
        b &= 0x1f;
        /* Rust release builds mask the shift count to the type width */
        switch (op_data_type(&ir.operands[0])) {
        case D_WORD:
            result = (uint32_t)((int32_t)a >> b);
            break;
        case D_UWORD:
            result = a >> b;
            break;
        case D_HALF:
            result = (uint32_t)(int32_t)(int16_t)((int16_t)a >> (b & 15));
            break;
        case D_UHALF:
            result = (uint16_t)((uint16_t)a >> (b & 15));
            break;
        case D_BYTE:
            result = (uint8_t)((uint8_t)a >> (b & 7));
            break;
        case D_SBYTE:
            result = (uint32_t)(int32_t)(int8_t)((int8_t)a >> (b & 7));
            break;
        default:
            result = 0;
            break;
        }
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case LRSW3:
        READ(1, a);
        READ(0, b);
        b &= 0x1f;
        result = a >> b;
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag_op(result, 2);
        break;
    case MCOMW:
    case MCOMH:
    case MCOMB:
        READ(0, a);
        result = ~a;
        TRY(write_op(1, result));
        set_nz_flags(result, 1);
        set_c_flag(false);
        set_v_flag_op(result, 1);
        break;
    case MNEGW:
    case MNEGH:
    case MNEGB:
        READ(0, a);
        result = ~a + 1;
        TRY(write_op(1, result));
        set_nz_flags(result, 1);
        set_c_flag(false);
        set_v_flag_op(result, 1);
        break;
    case MOVBLW:
        while (R[2] != 0) {
            TRY(bus_read_word(R[0], &a));
            TRY(bus_write_word(R[1], a));
            R[2] -= 1;
            R[0] += 4;
            R[1] += 4;
        }
        break;
    case STREND:
        for (;;) {
            uint8_t ch;
            TRY(bus_read_byte(R[0], &ch));
            if (ch == 0)
                break;
            R[0] += 1;
        }
        break;
    case SWAPWI:
    case SWAPHI:
    case SWAPBI:
        READ(0, a);
        TRY(write_op(0, R[0]));
        R[0] = a;
        set_n_flag((int32_t)a < 0);
        set_z_flag(a == 0);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case ROTW:
        READ(0, a);
        a &= 0x1f;
        READ(1, b);
        result = a ? (b >> a) | (b << (32 - a)) : b;
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case MOVAW:
        TRY(effective_address(0, &result));
        TRY(write_op(1, result));
        break;
    case MOVB:
    case MOVH:
    case MOVW:
        READ(0, val);
        TRY(write_op(1, val));
        set_nz_flags(val, 1);
        set_c_flag(false);
        set_v_flag_op(val, 1);
        break;
    case MOVTRW:
        TRY(effective_address(0, &val));
        TRY(write_op(1, val));
        set_nz_flags(val, 1);
        set_c_flag(false);
        set_v_flag_op(val, 1);
        break;
    case MODW2:
    case MODH2:
    case MODB2:
    case MODW3:
    case MODH3:
    case MODB3: {
        int dst = (ir.opcode == MODW2 || ir.opcode == MODH2 || ir.opcode == MODB2) ? 1 : 2;
        READ(0, a);
        READ(1, b);
        if (a == 0)
            return E_ZERO_DIVIDE;
        result = divmod(a, b, 1, true);
        TRY(write_op(dst, result));
        set_nz_flags(result, dst);
        set_c_flag(false);
        set_v_flag_op(result, dst);
        break;
    }
    case MULW2:
    case MULH2:
    case MULB2:
        READ(0, a);
        READ(1, b);
        result = a * b;
        TRY(write_op(1, result));
        set_nz_flags(result, 1);
        set_c_flag(false);
        set_v_flag_op(result, 1);
        break;
    case MULW3:
    case MULH3:
    case MULB3:
        READ(0, a);
        READ(1, b);
        result = a * b;
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag_op(result, 2);
        break;
    case ORW2:
    case ORH2:
    case ORB2:
        READ(0, a);
        READ(1, b);
        result = a | b;
        TRY(write_op(1, result));
        set_nz_flags(result, 1);
        set_c_flag(false);
        set_v_flag_op(result, 1);
        break;
    case ORW3:
    case ORH3:
    case ORB3:
        READ(0, a);
        READ(1, b);
        result = a | b;
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag_op(result, 2);
        break;
    case POPW:
        TRY(bus_read_word((addr_t)R[R_SP] - 4, &val));
        TRY(write_op(0, val));
        R[R_SP] -= 4;
        set_nz_flags(val, 0);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case PUSHAW:
        TRY(effective_address(0, &val));
        TRY(stack_push(val));
        set_nz_flags(val, 0);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case PUSHW:
        READ(0, val);
        TRY(stack_push(val));
        set_nz_flags(val, 0);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case RESTORE: {
        a = R[R_FP] - 28;
        TRY(bus_read_word(a, &b));
        c = R[R_FP] - 24;
        int r = ir.operands[0].reg;
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        while (r < R_FP) {
            TRY(bus_read_word(c, &R[r]));
            r++;
            c += 4;
        }
        R[R_FP] = b;
        R[R_SP] = a;
        break;
    }
    case RETG: {
        uint32_t new_psw, new_pc;
        TRY(bus_read_word((addr_t)R[R_SP] - 4, &new_psw));
        TRY(bus_read_word((addr_t)R[R_SP] - 8, &new_pc));
        /* TODO (Rust core): check for an illegal level change */
        new_psw &= ~(F_IPL | F_CFD | F_QIE | F_CD | F_R | F_ISC | F_TM | F_ET);
        new_psw |= R[R_PSW] & F_IPL;
        new_psw |= R[R_PSW] & F_CFD;
        new_psw |= R[R_PSW] & F_QIE;
        new_psw |= R[R_PSW] & F_CD;
        new_psw |= R[R_PSW] & F_R;
        new_psw |= 7 << O_ISC;
        new_psw |= 3 << O_ET;
        R[R_PSW] = new_psw;
        R[R_PC] = new_pc;
        R[R_SP] -= 8;
        pc_increment = 0;
        break;
    }
    case RGEQ:
    case RGEQU:
    case RGTR:
    case RNEQ:
    case RNEQU:
    case RLEQ:
    case RLEQU:
    case RLSS:
    case REQL:
    case REQLU:
    case RSB: {
        bool take;
        switch (ir.opcode) {
        case RGEQ: take = !n_flag() || z_flag(); break;
        case RGEQU: take = !c_flag(); break;
        case RGTR: take = !n_flag() && !z_flag(); break;
        case RNEQ:
        case RNEQU: take = !z_flag(); break;
        case RLEQ: take = n_flag() || z_flag(); break;
        case RLEQU: take = c_flag() || z_flag(); break;
        case RLSS: take = n_flag() || !z_flag(); break; /* sic, as in the Rust core */
        case REQL:
        case REQLU: take = z_flag(); break;
        default: take = true; break; /* RSB */
        }
        if (take) {
            TRY(stack_pop(&R[R_PC]));
            pc_increment = 0;
        }
        break;
    }
    case RET:
        a = R[R_AP];
        TRY(bus_read_word((uint32_t)(R[R_SP] - 4), &b));
        TRY(bus_read_word((uint32_t)(R[R_SP] - 8), &c));
        R[R_AP] = b;
        R[R_PC] = c;
        R[R_SP] = a;
        pc_increment = 0;
        break;
    case RETPS: {
        uint32_t new_pcbp, new_psw;
        if (!is_kernel())
            return E_PRIVILEGED_OPCODE;
        TRY(irq_pop(&new_pcbp));
        TRY(bus_read_word(new_pcbp, &new_psw));
        R[R_PSW] &= ~F_R;
        R[R_PSW] |= new_psw & F_R;
        TRY(context_switch_2(new_pcbp));
        TRY(context_switch_3());
        if (R[R_PSW] & F_R) {
            TRY(bus_read_word((uint32_t)(new_pcbp + 24), &R[R_FP]));
            for (int i = 0; i <= 8; i++)
                TRY(bus_read_word((uint32_t)(new_pcbp + 28 + 4 * i), &R[i]));
            TRY(bus_read_word((uint32_t)(new_pcbp + 20), &R[R_AP]));
        }
        pc_increment = 0;
        break;
    }
    case SAVE: {
        TRY(bus_write_word(R[R_SP], R[R_FP]));
        int r = ir.operands[0].reg;
        if (r < 0)
            return E_ILLEGAL_OPCODE;
        addr_t stack_offset = 4;
        while (r < R_FP) {
            TRY(bus_write_word((addr_t)R[R_SP] + stack_offset, R[r]));
            r++;
            stack_offset += 4;
        }
        R[R_SP] += 28;
        R[R_FP] = R[R_SP];
        break;
    }
    case SUBW2:
    case SUBH2:
    case SUBB2:
        READ(1, a);
        READ(0, b);
        TRY(sub(a, b, 1));
        break;
    case SUBW3:
    case SUBH3:
    case SUBB3:
        READ(1, a);
        READ(0, b);
        TRY(sub(a, b, 2));
        break;
    case TSTW:
        READ(0, a);
        set_n_flag((int32_t)a < 0);
        set_z_flag(a == 0);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case TSTH:
        READ(0, a);
        set_n_flag((int16_t)a < 0);
        set_z_flag(a == 0);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case TSTB:
        READ(0, a);
        set_n_flag((int8_t)a < 0);
        set_z_flag(a == 0);
        set_c_flag(false);
        set_v_flag(false);
        break;
    case XORW2:
    case XORH2:
    case XORB2:
        READ(0, a);
        READ(1, b);
        result = a ^ b;
        TRY(write_op(1, result));
        set_nz_flags(result, 1);
        set_c_flag(false);
        set_v_flag_op(result, 1);
        break;
    case XORW3:
    case XORH3:
    case XORB3:
        READ(0, a);
        READ(1, b);
        result = a ^ b;
        TRY(write_op(2, result));
        set_nz_flags(result, 2);
        set_c_flag(false);
        set_v_flag_op(result, 2);
        break;
    default:
        return E_ILLEGAL_OPCODE;
    }

    *pc_inc = pc_increment;
    return E_OK;
}

/* Run one instruction */
void cpu_step(void)
{
    int32_t inc = 0;
    int e = dispatch(&inc);

    switch (e) {
    case E_OK:
        R[R_PC] += (uint32_t)inc;
        break;
    case E_BUS_NODEV:
    case E_BUS_READ:
    case E_BUS_WRITE:
        if ((e = on_exception_external_memory()) != E_OK)
            core_fatal("exception handler failed: %s, PC=%08x", core_error_name(e), R[R_PC]);
        break;
    default:
        core_fatal("Unexpected CPU Error '%s'. PC=%08x R0=%08x R1=%08x R2=%08x OP=%s (%04x)",
                   core_error_name(e), R[R_PC], R[0], R[1], R[2], ir.name, ir.opcode);
    }
}

int cpu_step_with_error(void)
{
    int32_t inc = 0;
    TRY(dispatch(&inc));
    R[R_PC] += (uint32_t)inc;
    return E_OK;
}

/* ---- Test hooks (test/difftest) ---- */

int test_opcode_list(uint16_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < 256 && n < max; i++)
        if (byte_mnemonics[i].valid)
            out[n++] = byte_mnemonics[i].opcode;
    for (size_t i = 0; i < sizeof halfword_mnemonics / sizeof halfword_mnemonics[0] && n < max; i++)
        out[n++] = halfword_mnemonics[i].opcode;
    return n;
}

int test_operand_types(uint16_t opcode, uint8_t *dtype, uint8_t ot[4])
{
    if (opcode >= 0x100 || !byte_mnemonics[opcode].valid)
        return -1;
    *dtype = byte_mnemonics[opcode].dtype;
    memcpy(ot, byte_mnemonics[opcode].ops, 4);
    return 0;
}
