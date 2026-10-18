#ifndef EMU_MEM_H
#define EMU_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Address space of the fx-CG10/20/50 (SH7305: SH4AL-DSP core + peripherals).
 *
 * Physical memory (29 bits): ROM at 0x00000000 (32 MB), on-chip peripheral
 * registers in area 1 (0x04000000), RAM (where it is and how big depends on
 * the model: see Layout in emu.h) and the R61524 display controller at
 * 0x14000000. P1 (0x80000000) and P2 (0xA0000000) see physical memory
 * directly, which is how gint and the DMA reach it.
 *
 * U0 (below 0x80000000) goes through the MMU. The OS maps the add-in at
 * 0x00300000 and its static RAM at 0x08100000; we install those mappings once
 * (and describe them in the UTLB arrays for gint) instead of emulating TLB
 * refills. P4 (0xE0000000 and up) holds on-chip RAMs and control registers.
 */
#define ROM_PHYS      0x00000000u
#define ROM_SIZE      0x02000000u
#define LCD_PHYS      0x14000000u

#define P1(phys)      ((phys) | 0x80000000u)
#define P2(phys)      ((phys) | 0xA0000000u)

#define ADDIN_BASE    0x00300000u  /* g3a code (from file offset 0x7000), read-only */
#define ADDIN_MAX     0x00400000u  /* largest add-in we map */
#define ADDIN_PHYS    0x01000000u  /* where we put it in ROM */
#define URAM_BASE     0x08100000u  /* add-in static RAM: .data, .bss */
#define URAM_SIZE     0x00080000u

/* Magic addresses the CPU loop intercepts instead of executing. */
#define SYSCALL_ENTRY 0x80020070u  /* jump here with the syscall number in r0 */
#define EXIT_ADDR     0xDEAD0000u  /* PR when main() starts: returning here ends the run */

/* The OS draws 384x216 (VRAM); the panel behind it is 396x224. */
#define LCD_WIDTH     384
#define LCD_HEIGHT    216
#define VRAM_SIZE     (LCD_WIDTH * LCD_HEIGHT * 2)

#define PAGE_SHIFT    12
#define PAGE_SIZE     (1u << PAGE_SHIFT)
#define PAGE_COUNT    (1u << (32 - PAGE_SHIFT))

typedef enum {
    FAULT_NONE,
    FAULT_UNMAPPED,
    FAULT_MISALIGNED,
    FAULT_READONLY,
} FaultKind;

/* Memory-mapped device: called for every access to its pages. addr is the
 * full virtual address; size is 1, 2 or 4. */
typedef uint32_t (*MmioRead)(void *ctx, uint32_t addr, int size);
typedef void (*MmioWrite)(void *ctx, uint32_t addr, uint32_t value, int size);

typedef struct {
    uint32_t base, size;
    MmioRead read;
    MmioWrite write;
    void *ctx;
} MmioDevice;

/* Physical memory seen through P2, the uncached window: same bytes as P1,
 * but every access goes out to the bus (see mem_map_phys). */
typedef struct {
    uint8_t *host;
    uint32_t base, size;
    bool writable;
    struct Memory_ *mem;
} UncachedRegion;

typedef struct Memory_ {
    uint8_t **rd;        /* per page: host address of the page, NULL if not plain memory */
    uint8_t **wr;        /* same, NULL for read-only pages */
    uint8_t *mmio_of;    /* per page: index + 1 into mmio[], 0 if none */
    MmioDevice mmio[32];
    int mmio_count;
    uint8_t *blocks[64]; /* allocations owned by this Memory */
    int block_count;

    /* Unknown control registers get a zeroed scratch page on first access
     * (so save/restore of registers we don't model works). Logged once. */
    bool lazy_io;

    UncachedRegion uncached[4];
    int uncached_count;
    /* Extra CPU cycles per uncached access (0 = no cost, e.g. in tests), and
     * the stall accumulated since the CPU last collected it. */
    uint32_t uncached_read_cycles, uncached_write_cycles;
    uint32_t stall_cycles;
    bool trace_io;

    FaultKind fault;     /* set by the first faulting access, cleared by the CPU */
    uint32_t fault_addr;
    bool fault_write;
} Memory;

bool mem_init(Memory *mem);
void mem_free(Memory *mem);

/* Zeroed host buffer owned by mem (freed by mem_free). */
uint8_t *mem_alloc(Memory *mem, uint32_t size);

/* Maps [vaddr, vaddr+size) to host memory. Page-aligned. */
void mem_map(Memory *mem, uint32_t vaddr, uint32_t size, uint8_t *host, bool writable);
/* Maps physical memory at its P1 (cached, plain memory) and P2 (uncached:
 * each access adds uncached_*_cycles to stall_cycles) addresses. */
void mem_map_phys(Memory *mem, uint32_t paddr, uint32_t size, uint8_t *host, bool writable);
/* Convenience for tests: allocate and map RAM. */
uint8_t *mem_add_ram(Memory *mem, uint32_t vaddr, uint32_t size, bool writable);
bool mem_add_mmio(Memory *mem, uint32_t vaddr, uint32_t size, MmioRead read, MmioWrite write, void *ctx);

/* Host pointer to [addr, addr+len) if it is plain contiguous memory, else NULL. */
uint8_t *mem_ptr(Memory *mem, uint32_t addr, uint32_t len);

/* Slow paths (MMIO, faults); used by the inline accessors below. */
uint32_t mem_read_slow(Memory *mem, uint32_t addr, int size);
void mem_write_slow(Memory *mem, uint32_t addr, uint32_t value, int size);

/* SH is big-endian. Faulting accesses read as 0 and set mem->fault. */
static inline uint8_t mem_read8(Memory *mem, uint32_t addr)
{
    uint8_t *p = mem->rd[addr >> PAGE_SHIFT];
    return p ? p[addr & (PAGE_SIZE - 1)] : (uint8_t)mem_read_slow(mem, addr, 1);
}

static inline uint16_t mem_read16(Memory *mem, uint32_t addr)
{
    uint8_t *p = mem->rd[addr >> PAGE_SHIFT];
    if (!p || (addr & 1))
        return (uint16_t)mem_read_slow(mem, addr, 2);
    p += addr & (PAGE_SIZE - 1);
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t mem_read32(Memory *mem, uint32_t addr)
{
    uint8_t *p = mem->rd[addr >> PAGE_SHIFT];
    if (!p || (addr & 3))
        return mem_read_slow(mem, addr, 4);
    p += addr & (PAGE_SIZE - 1);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void mem_write8(Memory *mem, uint32_t addr, uint8_t value)
{
    uint8_t *p = mem->wr[addr >> PAGE_SHIFT];
    if (p)
        p[addr & (PAGE_SIZE - 1)] = value;
    else
        mem_write_slow(mem, addr, value, 1);
}

static inline void mem_write16(Memory *mem, uint32_t addr, uint16_t value)
{
    uint8_t *p = mem->wr[addr >> PAGE_SHIFT];
    if (!p || (addr & 1)) {
        mem_write_slow(mem, addr, value, 2);
        return;
    }
    p += addr & (PAGE_SIZE - 1);
    p[0] = value >> 8;
    p[1] = value & 0xFF;
}

static inline void mem_write32(Memory *mem, uint32_t addr, uint32_t value)
{
    uint8_t *p = mem->wr[addr >> PAGE_SHIFT];
    if (!p || (addr & 3)) {
        mem_write_slow(mem, addr, value, 4);
        return;
    }
    p += addr & (PAGE_SIZE - 1);
    p[0] = value >> 24;
    p[1] = value >> 16 & 0xFF;
    p[2] = value >> 8 & 0xFF;
    p[3] = value & 0xFF;
}

/* Copies a NUL-terminated string out of emulated memory (truncates at cap-1). */
void mem_read_cstr(Memory *mem, uint32_t addr, char *out, size_t cap);

/* Big-endian helpers for host buffers holding emulated memory. */
static inline uint32_t be_read(const uint8_t *p, int size)
{
    if (size == 1) return p[0];
    if (size == 2) return (uint32_t)(p[0] << 8 | p[1]);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void be_write(uint8_t *p, uint32_t v, int size)
{
    for (int i = size - 1; i >= 0; i--, v >>= 8)
        p[i] = v & 0xFF;
}

#endif
