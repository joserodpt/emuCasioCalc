#include "mem.h"

#include <stdio.h>
#include <stdlib.h>

bool mem_init(Memory *mem)
{
    *mem = (Memory){0};
    mem->rd = calloc(PAGE_COUNT, sizeof *mem->rd);
    mem->wr = calloc(PAGE_COUNT, sizeof *mem->wr);
    mem->mmio_of = calloc(PAGE_COUNT, 1);
    if (!mem->rd || !mem->wr || !mem->mmio_of) {
        mem_free(mem);
        return false;
    }
    return true;
}

void mem_free(Memory *mem)
{
    for (int i = 0; i < mem->block_count; i++)
        free(mem->blocks[i]);
    free(mem->rd);
    free(mem->wr);
    free(mem->mmio_of);
    *mem = (Memory){0};
}

uint8_t *mem_alloc(Memory *mem, uint32_t size)
{
    if (mem->block_count >= (int)(sizeof mem->blocks / sizeof mem->blocks[0]))
        return NULL;
    uint8_t *p = calloc(1, size);
    if (p)
        mem->blocks[mem->block_count++] = p;
    return p;
}

void mem_map(Memory *mem, uint32_t vaddr, uint32_t size, uint8_t *host, bool writable)
{
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        uint32_t page = (vaddr + off) >> PAGE_SHIFT;
        mem->rd[page] = host + off;
        mem->wr[page] = writable ? host + off : NULL;
        mem->mmio_of[page] = 0;
    }
}

static void fault(Memory *mem, uint32_t addr, FaultKind kind, bool write);

static uint32_t uncached_read(void *ctx, uint32_t addr, int size)
{
    UncachedRegion *r = ctx;
    r->mem->stall_cycles += r->mem->uncached_read_cycles;
    return be_read(r->host + (addr - r->base), size);
}

static void uncached_write(void *ctx, uint32_t addr, uint32_t value, int size)
{
    UncachedRegion *r = ctx;
    if (!r->writable) {
        fault(r->mem, addr, FAULT_READONLY, true);
        return;
    }
    r->mem->stall_cycles += r->mem->uncached_write_cycles;
    be_write(r->host + (addr - r->base), value, size);
}

void mem_map_phys(Memory *mem, uint32_t paddr, uint32_t size, uint8_t *host, bool writable)
{
    mem_map(mem, P1(paddr), size, host, writable);
    if (mem->uncached_count >= (int)(sizeof mem->uncached / sizeof mem->uncached[0])) {
        mem_map(mem, P2(paddr), size, host, writable); /* out of slots: no cost */
        return;
    }
    UncachedRegion *r = &mem->uncached[mem->uncached_count++];
    *r = (UncachedRegion){host, P2(paddr), size, writable, mem};
    mem_add_mmio(mem, P2(paddr), size, uncached_read, uncached_write, r);
}

uint8_t *mem_add_ram(Memory *mem, uint32_t vaddr, uint32_t size, bool writable)
{
    uint8_t *p = mem_alloc(mem, size);
    if (p)
        mem_map(mem, vaddr, size, p, writable);
    return p;
}

bool mem_add_mmio(Memory *mem, uint32_t vaddr, uint32_t size, MmioRead read, MmioWrite write, void *ctx)
{
    if (mem->mmio_count >= (int)(sizeof mem->mmio / sizeof mem->mmio[0]))
        return false;
    mem->mmio[mem->mmio_count++] = (MmioDevice){vaddr, size, read, write, ctx};
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        uint32_t page = (vaddr + off) >> PAGE_SHIFT;
        mem->rd[page] = mem->wr[page] = NULL;
        mem->mmio_of[page] = (uint8_t)mem->mmio_count;
    }
    return true;
}

uint8_t *mem_ptr(Memory *mem, uint32_t addr, uint32_t len)
{
    if (len == 0)
        len = 1;
    uint8_t *base = mem->rd[addr >> PAGE_SHIFT];
    if (!base || addr + len - 1 < addr)
        return NULL;
    base += addr & (PAGE_SIZE - 1);
    /* Every following page must continue the same host buffer. */
    uint32_t first = addr >> PAGE_SHIFT, last = (addr + len - 1) >> PAGE_SHIFT;
    for (uint32_t p = first + 1; p <= last; p++)
        if (mem->rd[p] != mem->rd[first] + (p - first) * PAGE_SIZE)
            return NULL;
    return base;
}

static void fault(Memory *mem, uint32_t addr, FaultKind kind, bool write)
{
    if (mem->fault)
        return;
    mem->fault = kind;
    mem->fault_addr = addr;
    mem->fault_write = write;
}

/* Control register areas where an unknown register gets a scratch page. */
static bool is_io(uint32_t addr)
{
    return (addr >= 0xA4000000u && addr < 0xA6000000u) || addr >= 0xF0000000u;
}

static bool lazy_page(Memory *mem, uint32_t addr)
{
    if (!mem->lazy_io || !is_io(addr))
        return false;
    uint8_t *page = mem_alloc(mem, PAGE_SIZE);
    if (!page)
        return false;
    if (mem->trace_io)
        fprintf(stderr, "memory: unmodelled I/O page 0x%08X (first access at 0x%08X)\n",
                addr & ~(PAGE_SIZE - 1), addr);
    mem_map(mem, addr & ~(PAGE_SIZE - 1), PAGE_SIZE, page, true);
    return true;
}

uint32_t mem_read_slow(Memory *mem, uint32_t addr, int size)
{
    if (addr & (uint32_t)(size - 1)) {
        fault(mem, addr, FAULT_MISALIGNED, false);
        return 0;
    }
    uint32_t page = addr >> PAGE_SHIFT;
    if (mem->mmio_of[page]) {
        MmioDevice *d = &mem->mmio[mem->mmio_of[page] - 1];
        return d->read ? d->read(d->ctx, addr, size) : 0;
    }
    if (mem->rd[page] || lazy_page(mem, addr))
        return be_read(mem->rd[page] + (addr & (PAGE_SIZE - 1)), size);
    fault(mem, addr, FAULT_UNMAPPED, false);
    return 0;
}

void mem_write_slow(Memory *mem, uint32_t addr, uint32_t value, int size)
{
    if (addr & (uint32_t)(size - 1)) {
        fault(mem, addr, FAULT_MISALIGNED, true);
        return;
    }
    uint32_t page = addr >> PAGE_SHIFT;
    if (mem->mmio_of[page]) {
        MmioDevice *d = &mem->mmio[mem->mmio_of[page] - 1];
        if (d->write)
            d->write(d->ctx, addr, value, size);
        return;
    }
    if (mem->rd[page] && !mem->wr[page]) {
        fault(mem, addr, FAULT_READONLY, true);
        return;
    }
    if (mem->wr[page] || lazy_page(mem, addr)) {
        be_write(mem->wr[page] + (addr & (PAGE_SIZE - 1)), value, size);
        return;
    }
    fault(mem, addr, FAULT_UNMAPPED, true);
}

void mem_read_cstr(Memory *mem, uint32_t addr, char *out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap) {
        uint8_t c = mem_read8(mem, addr + (uint32_t)i);
        if (!c || mem->fault)
            break;
        out[i++] = (char)c;
    }
    out[i] = '\0';
}
