#ifndef _NUMA_H
#define _NUMA_H

#include <CPU/ACPI.h>

#include <stdbool.h>
#include <stdint.h>

#define NUMA_MAX_NODES              16U
#define NUMA_MAX_MEM_RANGES         64U
#define NUMA_INVALID_NODE           0xFFFFFFFFU

#define NUMA_APIC_ID_MAP_SIZE           1024U
#define NUMA_DISTANCE_LOCAL_DEFAULT     10U
#define NUMA_DISTANCE_REMOTE_DEFAULT    20U

#define ACPI_SRAT_TYPE_CPU_AFFINITY     0U
#define ACPI_SRAT_TYPE_MEMORY_AFFINITY  1U
#define ACPI_SRAT_TYPE_X2APIC_AFFINITY  2U

#define ACPI_SRAT_FLAG_ENABLED          (1U << 0)
#define ACPI_SRAT_MEM_HOTPLUGGABLE      (1U << 1)
#define ACPI_SRAT_MEM_NON_VOLATILE      (1U << 2)

typedef struct NUMA_memory_range
{
    uint64_t base;
    uint64_t length;
    uint32_t node_id;
    uint8_t hotpluggable;
    uint8_t non_volatile;
    uint16_t reserved;
} NUMA_memory_range_t;

typedef struct ACPI_SRAT
{
    ACPI_SDT_header_t header;
    uint32_t reserved1;
    uint64_t reserved2;
    uint8_t entries[];
} __attribute__((__packed__)) ACPI_SRAT_t;

typedef struct ACPI_SRAT_entry_header
{
    uint8_t type;
    uint8_t length;
} __attribute__((__packed__)) ACPI_SRAT_entry_header_t;

typedef struct ACPI_SRAT_cpu_affinity
{
    uint8_t type;
    uint8_t length;
    uint8_t proximity_domain_low;
    uint8_t apic_id;
    uint32_t flags;
    uint8_t local_sapic_eid;
    uint8_t proximity_domain_high[3];
    uint32_t clock_domain;
} __attribute__((__packed__)) ACPI_SRAT_cpu_affinity_t;

typedef struct ACPI_SRAT_mem_affinity
{
    uint8_t type;
    uint8_t length;
    uint32_t proximity_domain;
    uint16_t reserved1;
    uint64_t base;
    uint64_t length_bytes;
    uint32_t reserved2;
    uint32_t flags;
    uint64_t reserved3;
} __attribute__((__packed__)) ACPI_SRAT_mem_affinity_t;

typedef struct ACPI_SRAT_x2apic_affinity
{
    uint8_t type;
    uint8_t length;
    uint16_t reserved1;
    uint32_t proximity_domain;
    uint32_t x2apic_id;
    uint32_t flags;
    uint32_t clock_domain;
    uint32_t reserved2;
} __attribute__((__packed__)) ACPI_SRAT_x2apic_affinity_t;

typedef struct ACPI_SLIT
{
    ACPI_SDT_header_t header;
    uint64_t locality_count;
    uint8_t matrix[];
} __attribute__((__packed__)) ACPI_SLIT_t;

void NUMA_init(void);
bool NUMA_is_available(void);
uint32_t NUMA_get_node_count(void);
uint32_t NUMA_get_cpu_node(uint32_t cpu_index);
uint32_t NUMA_get_apic_node(uint32_t apic_id);
uint32_t NUMA_get_distance(uint32_t from_node, uint32_t to_node);
uint32_t NUMA_get_memory_range_count(void);
const NUMA_memory_range_t* NUMA_get_memory_range(uint32_t index);

#endif
