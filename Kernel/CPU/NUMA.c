#include <CPU/NUMA.h>

#include <CPU/APIC.h>
#include <CPU/SMP.h>
#include <Debug/KDebug.h>

#include <string.h>

static bool NUMA_ready = false;
static bool NUMA_available = false;
static uint32_t NUMA_node_count = 0;
static uint32_t NUMA_node_domain[NUMA_MAX_NODES];
static uint32_t NUMA_cpu_node[SMP_MAX_CPUS];
static uint32_t NUMA_apic_node[NUMA_APIC_ID_MAP_SIZE];
static uint8_t NUMA_distance[NUMA_MAX_NODES][NUMA_MAX_NODES];
static NUMA_memory_range_t NUMA_mem_ranges[NUMA_MAX_MEM_RANGES];
static uint32_t NUMA_mem_range_count = 0;

static void NUMA_reset(void)
{
    NUMA_ready = false;
    NUMA_available = false;
    NUMA_node_count = 0;
    NUMA_mem_range_count = 0;

    for (uint32_t i = 0; i < NUMA_MAX_NODES; i++)
    {
        NUMA_node_domain[i] = NUMA_INVALID_NODE;
        for (uint32_t j = 0; j < NUMA_MAX_NODES; j++)
            NUMA_distance[i][j] = (i == j) ? NUMA_DISTANCE_LOCAL_DEFAULT : NUMA_DISTANCE_REMOTE_DEFAULT;
    }

    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++)
        NUMA_cpu_node[cpu] = 0;

    for (uint32_t apic = 0; apic < NUMA_APIC_ID_MAP_SIZE; apic++)
        NUMA_apic_node[apic] = 0;
}

static uint32_t NUMA_domain_to_slot(uint32_t domain, bool create)
{
    for (uint32_t i = 0; i < NUMA_node_count; i++)
    {
        if (NUMA_node_domain[i] == domain)
            return i;
    }

    if (!create || NUMA_node_count >= NUMA_MAX_NODES)
        return NUMA_INVALID_NODE;

    uint32_t slot = NUMA_node_count++;
    NUMA_node_domain[slot] = domain;
    return slot;
}

static uint32_t NUMA_slot_to_domain(uint32_t slot)
{
    if (slot >= NUMA_node_count || slot >= NUMA_MAX_NODES)
        return 0;
    return NUMA_node_domain[slot];
}

static uint32_t NUMA_cpu_index_from_apic(uint32_t apic_id)
{
    uint8_t core_count = APIC_get_core_count();
    for (uint32_t cpu = 0; cpu < core_count; cpu++)
    {
        if ((uint32_t) APIC_get_core_id((uint8_t) cpu) == apic_id)
            return cpu;
    }

    return NUMA_INVALID_NODE;
}

static uint32_t NUMA_cpu_affinity_domain(const ACPI_SRAT_cpu_affinity_t* cpu_aff)
{
    uint32_t domain = (uint32_t) cpu_aff->proximity_domain_low;
    domain |= ((uint32_t) cpu_aff->proximity_domain_high[0]) << 8;
    domain |= ((uint32_t) cpu_aff->proximity_domain_high[1]) << 16;
    domain |= ((uint32_t) cpu_aff->proximity_domain_high[2]) << 24;
    return domain;
}

static void NUMA_parse_srat(ACPI_SRAT_t* srat)
{
    if (!srat || srat->header.length < sizeof(ACPI_SRAT_t))
        return;

    uintptr_t start = (uintptr_t) srat->entries;
    uintptr_t end = (uintptr_t) srat + srat->header.length;
    uint32_t cpu_aff_count = 0;
    uint32_t x2apic_aff_count = 0;
    uint32_t mem_aff_count = 0;

    while (start + sizeof(ACPI_SRAT_entry_header_t) <= end)
    {
        ACPI_SRAT_entry_header_t* entry = (ACPI_SRAT_entry_header_t*) start;
        if (entry->length < sizeof(ACPI_SRAT_entry_header_t) || start + entry->length > end)
            break;

        switch (entry->type)
        {
            case ACPI_SRAT_TYPE_CPU_AFFINITY:
            {
                if (entry->length < sizeof(ACPI_SRAT_cpu_affinity_t))
                    break;

                ACPI_SRAT_cpu_affinity_t* cpu_aff = (ACPI_SRAT_cpu_affinity_t*) start;
                if ((cpu_aff->flags & ACPI_SRAT_FLAG_ENABLED) == 0)
                    break;

                uint32_t domain = NUMA_cpu_affinity_domain(cpu_aff);
                uint32_t slot = NUMA_domain_to_slot(domain, true);
                if (slot == NUMA_INVALID_NODE)
                    break;

                uint32_t apic_id = cpu_aff->apic_id;
                uint32_t cpu_index = NUMA_cpu_index_from_apic(apic_id);
                if (cpu_index < SMP_MAX_CPUS)
                    NUMA_cpu_node[cpu_index] = domain;
                if (apic_id < NUMA_APIC_ID_MAP_SIZE)
                    NUMA_apic_node[apic_id] = domain;

                cpu_aff_count++;
                break;
            }

            case ACPI_SRAT_TYPE_X2APIC_AFFINITY:
            {
                if (entry->length < sizeof(ACPI_SRAT_x2apic_affinity_t))
                    break;

                ACPI_SRAT_x2apic_affinity_t* x2 = (ACPI_SRAT_x2apic_affinity_t*) start;
                if ((x2->flags & ACPI_SRAT_FLAG_ENABLED) == 0)
                    break;

                uint32_t domain = x2->proximity_domain;
                uint32_t slot = NUMA_domain_to_slot(domain, true);
                if (slot == NUMA_INVALID_NODE)
                    break;

                uint32_t apic_id = x2->x2apic_id;
                uint32_t cpu_index = NUMA_cpu_index_from_apic(apic_id);
                if (cpu_index < SMP_MAX_CPUS)
                    NUMA_cpu_node[cpu_index] = domain;
                if (apic_id < NUMA_APIC_ID_MAP_SIZE)
                    NUMA_apic_node[apic_id] = domain;

                x2apic_aff_count++;
                break;
            }

            case ACPI_SRAT_TYPE_MEMORY_AFFINITY:
            {
                if (entry->length < sizeof(ACPI_SRAT_mem_affinity_t))
                    break;

                ACPI_SRAT_mem_affinity_t* mem_aff = (ACPI_SRAT_mem_affinity_t*) start;
                if ((mem_aff->flags & ACPI_SRAT_FLAG_ENABLED) == 0 || mem_aff->length_bytes == 0)
                    break;

                uint32_t domain = mem_aff->proximity_domain;
                uint32_t slot = NUMA_domain_to_slot(domain, true);
                if (slot == NUMA_INVALID_NODE || NUMA_mem_range_count >= NUMA_MAX_MEM_RANGES)
                    break;

                NUMA_memory_range_t* range = &NUMA_mem_ranges[NUMA_mem_range_count++];
                range->base = mem_aff->base;
                range->length = mem_aff->length_bytes;
                range->node_id = domain;
                range->hotpluggable = (mem_aff->flags & ACPI_SRAT_MEM_HOTPLUGGABLE) ? 1 : 0;
                range->non_volatile = (mem_aff->flags & ACPI_SRAT_MEM_NON_VOLATILE) ? 1 : 0;
                range->reserved = 0;
                mem_aff_count++;
                break;
            }

            default:
                break;
        }

        start += entry->length;
    }

    if (NUMA_node_count > 1)
        NUMA_available = true;

    kdebug_printf("[NUMA] SRAT parsed: nodes=%u cpu_aff=%u x2apic_aff=%u mem_aff=%u\n",
                  NUMA_node_count,
                  cpu_aff_count,
                  x2apic_aff_count,
                  mem_aff_count);
}

static void NUMA_parse_slit(ACPI_SLIT_t* slit)
{
    if (!slit || slit->header.length < sizeof(ACPI_SLIT_t))
        return;

    uint64_t localities = slit->locality_count;
    if (localities == 0)
        return;

    uint64_t matrix_bytes = localities * localities;
    uintptr_t matrix_start = (uintptr_t) slit->matrix;
    uintptr_t matrix_end = matrix_start + matrix_bytes;
    uintptr_t table_end = (uintptr_t) slit + slit->header.length;
    if (matrix_end > table_end)
        return;

    for (uint32_t i = 0; i < NUMA_node_count; i++)
    {
        uint32_t domain_i = NUMA_slot_to_domain(i);
        if ((uint64_t) domain_i >= localities)
            continue;

        for (uint32_t j = 0; j < NUMA_node_count; j++)
        {
            uint32_t domain_j = NUMA_slot_to_domain(j);
            if ((uint64_t) domain_j >= localities)
                continue;

            uint64_t idx = (uint64_t) domain_i * localities + domain_j;
            NUMA_distance[i][j] = slit->matrix[idx];
        }
    }
}

static void NUMA_log_summary(void)
{
    if (!NUMA_available)
    {
        kdebug_puts("[NUMA] disabled (single node or no SRAT)\n");
        return;
    }

    uint8_t core_count = APIC_get_core_count();
    for (uint32_t slot = 0; slot < NUMA_node_count; slot++)
    {
        uint32_t domain = NUMA_slot_to_domain(slot);
        uint32_t cpu_count = 0;
        uint64_t mem_total = 0;

        for (uint32_t cpu = 0; cpu < core_count; cpu++)
        {
            if (NUMA_cpu_node[cpu] == domain)
                cpu_count++;
        }

        for (uint32_t r = 0; r < NUMA_mem_range_count; r++)
        {
            if (NUMA_mem_ranges[r].node_id == domain)
                mem_total += NUMA_mem_ranges[r].length;
        }

        kdebug_printf("[NUMA] node=%u cpus=%u mem=%llu MiB\n",
                      domain,
                      cpu_count,
                      (unsigned long long) (mem_total >> 20));
    }
}

void NUMA_init(void)
{
    NUMA_reset();

    ACPI_SRAT_t* srat = (ACPI_SRAT_t*) ACPI_get_table(ACPI_SRAT_SIGNATURE);
    if (!srat)
    {
        NUMA_ready = true;
        NUMA_available = false;
        kdebug_puts("[NUMA] SRAT table not found\n");
        return;
    }

    NUMA_parse_srat(srat);
    ACPI_SLIT_t* slit = (ACPI_SLIT_t*) ACPI_get_table(ACPI_SLIT_SIGNATURE);
    if (slit)
        NUMA_parse_slit(slit);

    NUMA_ready = true;
    NUMA_log_summary();
}

bool NUMA_is_available(void)
{
    return NUMA_ready && NUMA_available;
}

uint32_t NUMA_get_node_count(void)
{
    if (!NUMA_ready || NUMA_node_count == 0)
        return 1;
    return NUMA_node_count;
}

uint32_t NUMA_get_cpu_node(uint32_t cpu_index)
{
    if (!NUMA_ready || cpu_index >= SMP_MAX_CPUS)
        return 0;

    return NUMA_cpu_node[cpu_index];
}

uint32_t NUMA_get_apic_node(uint32_t apic_id)
{
    if (!NUMA_ready || apic_id >= NUMA_APIC_ID_MAP_SIZE)
        return 0;

    return NUMA_apic_node[apic_id];
}

uint32_t NUMA_get_distance(uint32_t from_node, uint32_t to_node)
{
    if (!NUMA_ready)
        return (from_node == to_node) ? NUMA_DISTANCE_LOCAL_DEFAULT : NUMA_DISTANCE_REMOTE_DEFAULT;

    uint32_t from_slot = NUMA_domain_to_slot(from_node, false);
    uint32_t to_slot = NUMA_domain_to_slot(to_node, false);
    if (from_slot == NUMA_INVALID_NODE || to_slot == NUMA_INVALID_NODE)
        return (from_node == to_node) ? NUMA_DISTANCE_LOCAL_DEFAULT : NUMA_DISTANCE_REMOTE_DEFAULT;

    return NUMA_distance[from_slot][to_slot];
}

uint32_t NUMA_get_memory_range_count(void)
{
    return NUMA_mem_range_count;
}

const NUMA_memory_range_t* NUMA_get_memory_range(uint32_t index)
{
    if (index >= NUMA_mem_range_count)
        return NULL;

    return &NUMA_mem_ranges[index];
}
