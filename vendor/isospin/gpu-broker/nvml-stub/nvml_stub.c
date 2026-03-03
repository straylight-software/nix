/*
 * NVML Stub Library - Returns mock GPU data for testing
 * 
 * This implements just enough of the NVML API to make nvidia-smi happy.
 * Build: gcc -shared -fPIC -o libnvidia-ml.so.1 nvml_stub.c
 * 
 * When the gpu-broker is running, this stub queries real GPU data via
 * a Unix socket. Falls back to hardcoded values if broker is unavailable.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

/* NVML return codes */
typedef enum {
    NVML_SUCCESS = 0,
    NVML_ERROR_UNINITIALIZED = 1,
    NVML_ERROR_INVALID_ARGUMENT = 2,
    NVML_ERROR_NOT_SUPPORTED = 3,
    NVML_ERROR_NO_PERMISSION = 4,
    NVML_ERROR_ALREADY_INITIALIZED = 5,
    NVML_ERROR_NOT_FOUND = 6,
    NVML_ERROR_INSUFFICIENT_SIZE = 7,
    NVML_ERROR_INSUFFICIENT_POWER = 8,
    NVML_ERROR_DRIVER_NOT_LOADED = 9,
    NVML_ERROR_TIMEOUT = 10,
    NVML_ERROR_IRQ_ISSUE = 11,
    NVML_ERROR_LIBRARY_NOT_FOUND = 12,
    NVML_ERROR_FUNCTION_NOT_FOUND = 13,
    NVML_ERROR_CORRUPTED_INFOROM = 14,
    NVML_ERROR_GPU_IS_LOST = 15,
    NVML_ERROR_RESET_REQUIRED = 16,
    NVML_ERROR_OPERATING_SYSTEM = 17,
    NVML_ERROR_LIB_RM_VERSION_MISMATCH = 18,
    NVML_ERROR_UNKNOWN = 999
} nvmlReturn_t;

/* Device handle - opaque pointer */
typedef struct nvmlDevice_st* nvmlDevice_t;

/* Memory info */
typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

/* Utilization */
typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

/* PCI info */
typedef struct {
    char busIdLegacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char busId[32];
} nvmlPciInfo_t;

/* Temperature sensors */
typedef enum {
    NVML_TEMPERATURE_GPU = 0
} nvmlTemperatureSensors_t;

/* Clock types */
typedef enum {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM = 1,
    NVML_CLOCK_MEM = 2,
    NVML_CLOCK_VIDEO = 3
} nvmlClockType_t;

/* --- Mock GPU Configuration --- */
static const char* GPU_NAME = "NVIDIA RTX PRO 6000";
static const char* GPU_UUID = "GPU-12345678-1234-1234-1234-123456789abc";
static const char* GPU_SERIAL = "0123456789";
static const char* DRIVER_VERSION = "580.105.08";
static const char* NVML_VERSION = "580.105.08";
static const char* CUDA_VERSION = "12.8";
static const unsigned long long GPU_MEMORY_TOTAL = 128ULL * 1024 * 1024 * 1024; /* 128 GB */
static const unsigned long long GPU_MEMORY_USED = 512ULL * 1024 * 1024; /* 512 MB */
static const unsigned int GPU_TEMPERATURE = 45;
static const unsigned int GPU_POWER_USAGE = 75000; /* 75W in milliwatts */
static const unsigned int GPU_POWER_LIMIT = 350000; /* 350W */
static const unsigned int GPU_FAN_SPEED = 30; /* 30% */
static const unsigned int GPU_CLOCK_GRAPHICS = 2520;
static const unsigned int GPU_CLOCK_MEMORY = 2250;

/* Mock device - just use a fixed pointer value */
static struct nvmlDevice_st { int dummy; } mock_device;
static int initialized = 0;

/* =============================================================================
 * BROKER PROTOCOL (must match gpu-broker/src/nvml_server.rs)
 * ============================================================================= */

#define NVML_BROKER_MAGIC 0x4C4D564E  /* "NVML" in little endian */
#define NVML_BROKER_VERSION 1
#define NVML_BROKER_SOCKET_PATH "/run/gpu-broker-nvml.sock"
#define NVML_BROKER_SOCKET_PATH_TMP "/tmp/gpu-broker-nvml.sock"

/* Query types */
#define NVML_QUERY_GPU_INFO 1

/* Request header (8 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t query_type;
} nvml_query_request_t;

/* GPU info response (256 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t status;
    char name[64];
    char uuid[48];
    uint64_t memory_total;
    uint64_t memory_free;
    uint64_t memory_used;
    uint32_t temperature;
    uint32_t power_usage;
    uint32_t power_limit;
    uint32_t gpu_utilization;
    uint32_t memory_utilization;
    uint32_t fan_speed;
    uint32_t pcie_gen;
    uint32_t pcie_width;
    uint32_t compute_major;
    uint32_t compute_minor;
    uint32_t sm_count;
    char driver_version[32];
    char cuda_version[16];
    uint8_t _pad[20];  /* 236 base + 20 = 256 bytes */
} nvml_gpu_info_response_t;

/* Cached broker response */
static int broker_connected = 0;
static nvml_gpu_info_response_t cached_gpu_info;

/* Connect to broker and query GPU info */
static int query_broker_gpu_info(void) {
    int sock;
    struct sockaddr_un addr;
    nvml_query_request_t request;
    ssize_t n;
    const char* socket_paths[] = {
        NVML_BROKER_SOCKET_PATH,
        NVML_BROKER_SOCKET_PATH_TMP,
        NULL
    };
    
    /* Try each socket path */
    for (int i = 0; socket_paths[i] != NULL; i++) {
        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) continue;
        
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_paths[i], sizeof(addr.sun_path) - 1);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            /* Connected! Send query */
            request.magic = NVML_BROKER_MAGIC;
            request.version = NVML_BROKER_VERSION;
            request.query_type = NVML_QUERY_GPU_INFO;
            
            n = write(sock, &request, sizeof(request));
            if (n == sizeof(request)) {
                /* Read response */
                n = read(sock, &cached_gpu_info, sizeof(cached_gpu_info));
                close(sock);
                
                if (n == sizeof(cached_gpu_info) && 
                    cached_gpu_info.magic == NVML_BROKER_MAGIC &&
                    cached_gpu_info.status == 0) {
                    broker_connected = 1;
                    return 1;
                }
            } else {
                close(sock);
            }
        } else {
            close(sock);
        }
    }
    
    broker_connected = 0;
    return 0;
}

/* === Initialization === */

nvmlReturn_t nvmlInit(void) {
    initialized = 1;
    /* Try to connect to broker for real GPU data */
    query_broker_gpu_info();
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlInit_v2(void) {
    return nvmlInit();
}

nvmlReturn_t nvmlInitWithFlags(unsigned int flags) {
    (void)flags;
    return nvmlInit();
}

nvmlReturn_t nvmlShutdown(void) {
    initialized = 0;
    return NVML_SUCCESS;
}

/* === System Queries === */

nvmlReturn_t nvmlSystemGetDriverVersion(char* version, unsigned int length) {
    if (!version || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.driver_version[0] != '\0') {
        strncpy(version, cached_gpu_info.driver_version, length);
    } else {
        strncpy(version, DRIVER_VERSION, length);
    }
    version[length-1] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetNVMLVersion(char* version, unsigned int length) {
    if (!version || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(version, NVML_VERSION, length);
    version[length-1] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetCudaDriverVersion(int* cudaDriverVersion) {
    if (!cudaDriverVersion) return NVML_ERROR_INVALID_ARGUMENT;
    *cudaDriverVersion = 12080; /* CUDA 12.8 */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetCudaDriverVersion_v2(int* cudaDriverVersion) {
    return nvmlSystemGetCudaDriverVersion(cudaDriverVersion);
}

/* === Device Enumeration === */

nvmlReturn_t nvmlDeviceGetCount(unsigned int* deviceCount) {
    if (!deviceCount) return NVML_ERROR_INVALID_ARGUMENT;
    *deviceCount = 1; /* One mock GPU */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int* deviceCount) {
    return nvmlDeviceGetCount(deviceCount);
}

nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t* device) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (index != 0) return NVML_ERROR_NOT_FOUND;
    *device = &mock_device;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t* device) {
    return nvmlDeviceGetHandleByIndex(index, device);
}

nvmlReturn_t nvmlDeviceGetHandleByUUID(const char* uuid, nvmlDevice_t* device) {
    if (!uuid || !device) return NVML_ERROR_INVALID_ARGUMENT;
    if (strcmp(uuid, GPU_UUID) != 0) return NVML_ERROR_NOT_FOUND;
    *device = &mock_device;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHandleByPciBusId(const char* pciBusId, nvmlDevice_t* device) {
    if (!pciBusId || !device) return NVML_ERROR_INVALID_ARGUMENT;
    *device = &mock_device;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char* pciBusId, nvmlDevice_t* device) {
    return nvmlDeviceGetHandleByPciBusId(pciBusId, device);
}

/* === Device Properties === */

nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char* name, unsigned int length) {
    if (!device || !name || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.name[0] != '\0') {
        strncpy(name, cached_gpu_info.name, length);
    } else {
        strncpy(name, GPU_NAME, length);
    }
    name[length-1] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char* uuid, unsigned int length) {
    if (!device || !uuid || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.uuid[0] != '\0') {
        strncpy(uuid, cached_gpu_info.uuid, length);
    } else {
        strncpy(uuid, GPU_UUID, length);
    }
    uuid[length-1] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetSerial(nvmlDevice_t device, char* serial, unsigned int length) {
    if (!device || !serial || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(serial, GPU_SERIAL, length);
    serial[length-1] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetIndex(nvmlDevice_t device, unsigned int* index) {
    if (!device || !index) return NVML_ERROR_INVALID_ARGUMENT;
    *index = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPciInfo(nvmlDevice_t device, nvmlPciInfo_t* pci) {
    if (!device || !pci) return NVML_ERROR_INVALID_ARGUMENT;
    memset(pci, 0, sizeof(*pci));
    pci->domain = 0;
    pci->bus = 1;
    pci->device = 0;
    pci->pciDeviceId = 0x290010DE; /* Device ID 0x2900, Vendor 0x10DE */
    pci->pciSubSystemId = 0x00000000;
    strcpy(pci->busIdLegacy, "0000:01:00.0");
    strcpy(pci->busId, "00000000:01:00.0");
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPciInfo_v2(nvmlDevice_t device, nvmlPciInfo_t* pci) {
    return nvmlDeviceGetPciInfo(device, pci);
}

nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t device, nvmlPciInfo_t* pci) {
    return nvmlDeviceGetPciInfo(device, pci);
}

nvmlReturn_t nvmlDeviceGetMinorNumber(nvmlDevice_t device, unsigned int* minorNumber) {
    if (!device || !minorNumber) return NVML_ERROR_INVALID_ARGUMENT;
    *minorNumber = 0;
    return NVML_SUCCESS;
}

/* === Memory === */

nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t* memory) {
    if (!device || !memory) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.memory_total > 0) {
        memory->total = cached_gpu_info.memory_total;
        memory->used = cached_gpu_info.memory_used;
        memory->free = cached_gpu_info.memory_free;
    } else {
        memory->total = GPU_MEMORY_TOTAL;
        memory->used = GPU_MEMORY_USED;
        memory->free = GPU_MEMORY_TOTAL - GPU_MEMORY_USED;
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_t* memory) {
    return nvmlDeviceGetMemoryInfo(device, memory);
}

/* === Utilization === */

nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t* utilization) {
    if (!device || !utilization) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected) {
        utilization->gpu = cached_gpu_info.gpu_utilization;
        utilization->memory = cached_gpu_info.memory_utilization;
    } else {
        utilization->gpu = 0;  /* 0% - idle */
        utilization->memory = 0;
    }
    return NVML_SUCCESS;
}

/* === Temperature === */

nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, nvmlTemperatureSensors_t sensorType, unsigned int* temp) {
    if (!device || !temp) return NVML_ERROR_INVALID_ARGUMENT;
    (void)sensorType;
    if (broker_connected && cached_gpu_info.temperature > 0) {
        *temp = cached_gpu_info.temperature;
    } else {
        *temp = GPU_TEMPERATURE;
    }
    return NVML_SUCCESS;
}

/* === Power === */

nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int* power) {
    if (!device || !power) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.power_usage > 0) {
        *power = cached_gpu_info.power_usage;
    } else {
        *power = GPU_POWER_USAGE;
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPowerManagementLimit(nvmlDevice_t device, unsigned int* limit) {
    if (!device || !limit) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.power_limit > 0) {
        *limit = cached_gpu_info.power_limit;
    } else {
        *limit = GPU_POWER_LIMIT;
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t device, unsigned int* limit) {
    if (!device || !limit) return NVML_ERROR_INVALID_ARGUMENT;
    *limit = GPU_POWER_LIMIT;
    return NVML_SUCCESS;
}

/* === Fan === */

nvmlReturn_t nvmlDeviceGetFanSpeed(nvmlDevice_t device, unsigned int* speed) {
    if (!device || !speed) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.fan_speed > 0) {
        *speed = cached_gpu_info.fan_speed;
    } else {
        *speed = GPU_FAN_SPEED;
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetFanSpeed_v2(nvmlDevice_t device, unsigned int fan, unsigned int* speed) {
    (void)fan;
    return nvmlDeviceGetFanSpeed(device, speed);
}

nvmlReturn_t nvmlDeviceGetNumFans(nvmlDevice_t device, unsigned int* numFans) {
    if (!device || !numFans) return NVML_ERROR_INVALID_ARGUMENT;
    *numFans = 1;
    return NVML_SUCCESS;
}

/* === Clocks === */

nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t device, nvmlClockType_t type, unsigned int* clock) {
    if (!device || !clock) return NVML_ERROR_INVALID_ARGUMENT;
    switch (type) {
        case NVML_CLOCK_GRAPHICS:
        case NVML_CLOCK_SM:
            *clock = GPU_CLOCK_GRAPHICS;
            break;
        case NVML_CLOCK_MEM:
            *clock = GPU_CLOCK_MEMORY;
            break;
        default:
            *clock = 0;
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMaxClockInfo(nvmlDevice_t device, nvmlClockType_t type, unsigned int* clock) {
    return nvmlDeviceGetClockInfo(device, type, clock);
}

/* === Compute Mode === */

typedef enum {
    NVML_COMPUTEMODE_DEFAULT = 0,
    NVML_COMPUTEMODE_EXCLUSIVE_THREAD = 1,
    NVML_COMPUTEMODE_PROHIBITED = 2,
    NVML_COMPUTEMODE_EXCLUSIVE_PROCESS = 3
} nvmlComputeMode_t;

nvmlReturn_t nvmlDeviceGetComputeMode(nvmlDevice_t device, nvmlComputeMode_t* mode) {
    if (!device || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = NVML_COMPUTEMODE_DEFAULT;
    return NVML_SUCCESS;
}

/* === Persistence Mode === */

typedef enum {
    NVML_FEATURE_DISABLED = 0,
    NVML_FEATURE_ENABLED = 1
} nvmlEnableState_t;

nvmlReturn_t nvmlDeviceGetPersistenceMode(nvmlDevice_t device, nvmlEnableState_t* mode) {
    if (!device || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* === Display === */

nvmlReturn_t nvmlDeviceGetDisplayActive(nvmlDevice_t device, nvmlEnableState_t* isActive) {
    if (!device || !isActive) return NVML_ERROR_INVALID_ARGUMENT;
    *isActive = NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetDisplayMode(nvmlDevice_t device, nvmlEnableState_t* display) {
    if (!device || !display) return NVML_ERROR_INVALID_ARGUMENT;
    *display = NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

/* === Performance State === */

typedef enum {
    NVML_PSTATE_0 = 0,  /* Maximum performance */
    NVML_PSTATE_15 = 15 /* Minimum performance */
} nvmlPstates_t;

nvmlReturn_t nvmlDeviceGetPerformanceState(nvmlDevice_t device, nvmlPstates_t* pState) {
    if (!device || !pState) return NVML_ERROR_INVALID_ARGUMENT;
    *pState = NVML_PSTATE_0;
    return NVML_SUCCESS;
}

/* === ECC === */

nvmlReturn_t nvmlDeviceGetEccMode(nvmlDevice_t device, nvmlEnableState_t* current, nvmlEnableState_t* pending) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (current) *current = NVML_FEATURE_ENABLED;
    if (pending) *pending = NVML_FEATURE_ENABLED;
    return NVML_SUCCESS;
}

/* === CUDA Compute Capability === */

nvmlReturn_t nvmlDeviceGetCudaComputeCapability(nvmlDevice_t device, int* major, int* minor) {
    if (!device || !major || !minor) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.compute_major > 0) {
        *major = (int)cached_gpu_info.compute_major;
        *minor = (int)cached_gpu_info.compute_minor;
    } else {
        *major = 10;  /* Blackwell */
        *minor = 0;
    }
    return NVML_SUCCESS;
}

/* === Board Info === */

nvmlReturn_t nvmlDeviceGetBoardId(nvmlDevice_t device, unsigned int* boardId) {
    if (!device || !boardId) return NVML_ERROR_INVALID_ARGUMENT;
    *boardId = 0x2900;
    return NVML_SUCCESS;
}

/* === Multi-GPU === */

nvmlReturn_t nvmlDeviceGetMultiGpuBoard(nvmlDevice_t device, unsigned int* multiGpuBool) {
    if (!device || !multiGpuBool) return NVML_ERROR_INVALID_ARGUMENT;
    *multiGpuBool = 0;
    return NVML_SUCCESS;
}

/* === Architecture === */

typedef enum {
    NVML_DEVICE_ARCH_KEPLER = 2,
    NVML_DEVICE_ARCH_MAXWELL = 3,
    NVML_DEVICE_ARCH_PASCAL = 4,
    NVML_DEVICE_ARCH_VOLTA = 5,
    NVML_DEVICE_ARCH_TURING = 6,
    NVML_DEVICE_ARCH_AMPERE = 7,
    NVML_DEVICE_ARCH_ADA = 8,
    NVML_DEVICE_ARCH_HOPPER = 9,
    NVML_DEVICE_ARCH_BLACKWELL = 10
} nvmlDeviceArchitecture_t;

nvmlReturn_t nvmlDeviceGetArchitecture(nvmlDevice_t device, nvmlDeviceArchitecture_t* arch) {
    if (!device || !arch) return NVML_ERROR_INVALID_ARGUMENT;
    *arch = NVML_DEVICE_ARCH_BLACKWELL;
    return NVML_SUCCESS;
}

/* === Processes === */

typedef struct {
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
} nvmlProcessInfo_t;

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses(nvmlDevice_t device, unsigned int* infoCount, nvmlProcessInfo_t* infos) {
    if (!device || !infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    *infoCount = 0; /* No processes running */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v2(nvmlDevice_t device, unsigned int* infoCount, nvmlProcessInfo_t* infos) {
    return nvmlDeviceGetComputeRunningProcesses(device, infoCount, infos);
}

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t device, unsigned int* infoCount, nvmlProcessInfo_t* infos) {
    return nvmlDeviceGetComputeRunningProcesses(device, infoCount, infos);
}

nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses(nvmlDevice_t device, unsigned int* infoCount, nvmlProcessInfo_t* infos) {
    if (!device || !infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    *infoCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v2(nvmlDevice_t device, unsigned int* infoCount, nvmlProcessInfo_t* infos) {
    return nvmlDeviceGetGraphicsRunningProcesses(device, infoCount, infos);
}

nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v3(nvmlDevice_t device, unsigned int* infoCount, nvmlProcessInfo_t* infos) {
    return nvmlDeviceGetGraphicsRunningProcesses(device, infoCount, infos);
}

/* === Internal Export Table (nvidia-smi uses this for version check) === */

/* Export table GUID for version 1 */
static const unsigned char NVML_EXPORT_TABLE_GUID_V1[16] = {
    0xa1, 0x57, 0x4b, 0x69, 0x97, 0x7d, 0xcb, 0x4a,
    0x84, 0xa2, 0x7d, 0x89, 0x1c, 0x1e, 0x8d, 0x34
};

/* This is how nvidia-smi checks if the library version matches */
nvmlReturn_t nvmlInternalGetExportTable(const void** exportTable, const unsigned char* guid) {
    (void)guid;
    /* Return success with NULL table - nvidia-smi falls back to normal API */
    if (exportTable) *exportTable = NULL;
    return NVML_SUCCESS;
}

/* === Error Strings === */

const char* nvmlErrorString(nvmlReturn_t result) {
    switch (result) {
        case NVML_SUCCESS: return "Success";
        case NVML_ERROR_UNINITIALIZED: return "Uninitialized";
        case NVML_ERROR_INVALID_ARGUMENT: return "Invalid Argument";
        case NVML_ERROR_NOT_SUPPORTED: return "Not Supported";
        case NVML_ERROR_NO_PERMISSION: return "No Permission";
        case NVML_ERROR_NOT_FOUND: return "Not Found";
        case NVML_ERROR_DRIVER_NOT_LOADED: return "Driver Not Loaded";
        default: return "Unknown Error";
    }
}

/* === Stubs for less common functions === */

nvmlReturn_t nvmlDeviceGetBrand(nvmlDevice_t device, unsigned int* type) {
    if (!device || !type) return NVML_ERROR_INVALID_ARGUMENT;
    *type = 0; /* NVML_BRAND_UNKNOWN - but nvidia-smi works */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetInforomVersion(nvmlDevice_t device, unsigned int object, char* version, unsigned int length) {
    if (!device || !version) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(version, "N/A", length);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetVbiosVersion(nvmlDevice_t device, char* version, unsigned int length) {
    if (!device || !version) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(version, "96.00.89.00.01", length);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetBoardPartNumber(nvmlDevice_t device, char* partNumber, unsigned int length) {
    if (!device || !partNumber) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(partNumber, "900-2G190-0000-000", length);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetBAR1MemoryInfo(nvmlDevice_t device, nvmlMemory_t* bar1Memory) {
    if (!device || !bar1Memory) return NVML_ERROR_INVALID_ARGUMENT;
    bar1Memory->total = 256ULL * 1024 * 1024 * 1024; /* 256GB BAR1 */
    bar1Memory->used = 1ULL * 1024 * 1024; /* 1MB used */
    bar1Memory->free = bar1Memory->total - bar1Memory->used;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCurrentClocksThrottleReasons(nvmlDevice_t device, unsigned long long* clocksThrottleReasons) {
    if (!device || !clocksThrottleReasons) return NVML_ERROR_INVALID_ARGUMENT;
    *clocksThrottleReasons = 0; /* No throttling */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetSupportedClocksThrottleReasons(nvmlDevice_t device, unsigned long long* supportedClocksThrottleReasons) {
    if (!device || !supportedClocksThrottleReasons) return NVML_ERROR_INVALID_ARGUMENT;
    *supportedClocksThrottleReasons = 0xFFFFFFFF;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPowerManagementMode(nvmlDevice_t device, nvmlEnableState_t* mode) {
    if (!device || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = NVML_FEATURE_ENABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetDefaultPowerManagementLimit(nvmlDevice_t device, unsigned int* defaultLimit) {
    if (!device || !defaultLimit) return NVML_ERROR_INVALID_ARGUMENT;
    *defaultLimit = GPU_POWER_LIMIT;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPowerManagementLimitConstraints(nvmlDevice_t device, unsigned int* minLimit, unsigned int* maxLimit) {
    if (!device || !minLimit || !maxLimit) return NVML_ERROR_INVALID_ARGUMENT;
    *minLimit = 100000;  /* 100W */
    *maxLimit = 450000;  /* 450W */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetTotalEnergyConsumption(nvmlDevice_t device, unsigned long long* energy) {
    if (!device || !energy) return NVML_ERROR_INVALID_ARGUMENT;
    *energy = 1000000; /* 1000 Joules */
    return NVML_SUCCESS;
}

/* MIG mode - not supported on this "GPU" */
nvmlReturn_t nvmlDeviceGetMigMode(nvmlDevice_t device, unsigned int* currentMode, unsigned int* pendingMode) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (currentMode) *currentMode = 0;
    if (pendingMode) *pendingMode = 0;
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Driver model (Windows only) */
nvmlReturn_t nvmlDeviceGetDriverModel(nvmlDevice_t device, unsigned int* current, unsigned int* pending) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Remapped rows */
nvmlReturn_t nvmlDeviceGetRemappedRows(nvmlDevice_t device, unsigned int* corrRows, unsigned int* uncRows, unsigned int* isPending, unsigned int* failureOccurred) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (corrRows) *corrRows = 0;
    if (uncRows) *uncRows = 0;
    if (isPending) *isPending = 0;
    if (failureOccurred) *failureOccurred = 0;
    return NVML_SUCCESS;
}

/* === Additional functions needed for nvidia-smi === */

/* PCIe Link Info */
nvmlReturn_t nvmlDeviceGetCurrPcieLinkGeneration(nvmlDevice_t device, unsigned int* currLinkGen) {
    if (!device || !currLinkGen) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.pcie_gen > 0) {
        *currLinkGen = cached_gpu_info.pcie_gen;
    } else {
        *currLinkGen = 5; /* PCIe Gen 5 */
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCurrPcieLinkWidth(nvmlDevice_t device, unsigned int* currLinkWidth) {
    if (!device || !currLinkWidth) return NVML_ERROR_INVALID_ARGUMENT;
    if (broker_connected && cached_gpu_info.pcie_width > 0) {
        *currLinkWidth = cached_gpu_info.pcie_width;
    } else {
        *currLinkWidth = 16; /* x16 */
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMaxPcieLinkGeneration(nvmlDevice_t device, unsigned int* maxLinkGen) {
    if (!device || !maxLinkGen) return NVML_ERROR_INVALID_ARGUMENT;
    *maxLinkGen = 5;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMaxPcieLinkWidth(nvmlDevice_t device, unsigned int* maxLinkWidth) {
    if (!device || !maxLinkWidth) return NVML_ERROR_INVALID_ARGUMENT;
    *maxLinkWidth = 16;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGpuMaxPcieLinkGeneration(nvmlDevice_t device, unsigned int* maxLinkGenDevice) {
    if (!device || !maxLinkGenDevice) return NVML_ERROR_INVALID_ARGUMENT;
    *maxLinkGenDevice = 5;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPcieThroughput(nvmlDevice_t device, unsigned int counter, unsigned int* value) {
    if (!device || !value) return NVML_ERROR_INVALID_ARGUMENT;
    *value = 0; /* No traffic */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPcieReplayCounter(nvmlDevice_t device, unsigned int* value) {
    if (!device || !value) return NVML_ERROR_INVALID_ARGUMENT;
    *value = 0;
    return NVML_SUCCESS;
}

/* Accounting Mode */
nvmlReturn_t nvmlDeviceGetAccountingMode(nvmlDevice_t device, nvmlEnableState_t* mode) {
    if (!device || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetAccountingMode(nvmlDevice_t device, nvmlEnableState_t mode) {
    (void)device; (void)mode;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetAccountingPids(nvmlDevice_t device, unsigned int* count, unsigned int* pids) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetAccountingStats(nvmlDevice_t device, unsigned int pid, void* stats) {
    (void)device; (void)pid; (void)stats;
    return NVML_ERROR_NOT_FOUND;
}

nvmlReturn_t nvmlDeviceGetAccountingBufferSize(nvmlDevice_t device, unsigned int* bufferSize) {
    if (!device || !bufferSize) return NVML_ERROR_INVALID_ARGUMENT;
    *bufferSize = 4096;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceClearAccountingPids(nvmlDevice_t device) {
    (void)device;
    return NVML_SUCCESS;
}

/* Auto Boosted Clocks */
nvmlReturn_t nvmlDeviceGetAutoBoostedClocksEnabled(nvmlDevice_t device, nvmlEnableState_t* isEnabled, nvmlEnableState_t* defaultIsEnabled) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (isEnabled) *isEnabled = NVML_FEATURE_ENABLED;
    if (defaultIsEnabled) *defaultIsEnabled = NVML_FEATURE_ENABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetAutoBoostedClocksEnabled(nvmlDevice_t device, nvmlEnableState_t enabled) {
    (void)device; (void)enabled;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetDefaultAutoBoostedClocksEnabled(nvmlDevice_t device, nvmlEnableState_t enabled, unsigned int flags) {
    (void)device; (void)enabled; (void)flags;
    return NVML_SUCCESS;
}

/* Application Clocks */
nvmlReturn_t nvmlDeviceGetApplicationsClock(nvmlDevice_t device, nvmlClockType_t clockType, unsigned int* clockMHz) {
    if (!device || !clockMHz) return NVML_ERROR_INVALID_ARGUMENT;
    switch (clockType) {
        case NVML_CLOCK_GRAPHICS: *clockMHz = GPU_CLOCK_GRAPHICS; break;
        case NVML_CLOCK_MEM: *clockMHz = GPU_CLOCK_MEMORY; break;
        default: *clockMHz = 0;
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetDefaultApplicationsClock(nvmlDevice_t device, nvmlClockType_t clockType, unsigned int* clockMHz) {
    return nvmlDeviceGetApplicationsClock(device, clockType, clockMHz);
}

nvmlReturn_t nvmlDeviceSetApplicationsClocks(nvmlDevice_t device, unsigned int memClockMHz, unsigned int graphicsClockMHz) {
    (void)device; (void)memClockMHz; (void)graphicsClockMHz;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceResetApplicationsClocks(nvmlDevice_t device) {
    (void)device;
    return NVML_SUCCESS;
}

/* Clock (generic) */
nvmlReturn_t nvmlDeviceGetClock(nvmlDevice_t device, nvmlClockType_t clockType, unsigned int clockId, unsigned int* clockMHz) {
    if (!device || !clockMHz) return NVML_ERROR_INVALID_ARGUMENT;
    switch (clockType) {
        case NVML_CLOCK_GRAPHICS: *clockMHz = GPU_CLOCK_GRAPHICS; break;
        case NVML_CLOCK_MEM: *clockMHz = GPU_CLOCK_MEMORY; break;
        default: *clockMHz = 0;
    }
    return NVML_SUCCESS;
}

/* Power State */
nvmlReturn_t nvmlDeviceGetPowerState(nvmlDevice_t device, nvmlPstates_t* pState) {
    if (!device || !pState) return NVML_ERROR_INVALID_ARGUMENT;
    *pState = NVML_PSTATE_0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPowerManagementDefaultLimit(nvmlDevice_t device, unsigned int* defaultLimit) {
    if (!device || !defaultLimit) return NVML_ERROR_INVALID_ARGUMENT;
    *defaultLimit = GPU_POWER_LIMIT;
    return NVML_SUCCESS;
}

/* Encoder/Decoder Utilization */
nvmlReturn_t nvmlDeviceGetEncoderUtilization(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs) {
    if (!device || !utilization || !samplingPeriodUs) return NVML_ERROR_INVALID_ARGUMENT;
    *utilization = 0;
    *samplingPeriodUs = 100000;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetDecoderUtilization(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs) {
    if (!device || !utilization || !samplingPeriodUs) return NVML_ERROR_INVALID_ARGUMENT;
    *utilization = 0;
    *samplingPeriodUs = 100000;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetEncoderStats(nvmlDevice_t device, unsigned int* sessionCount, unsigned int* averageFps, unsigned int* averageLatency) {
    if (!device || !sessionCount || !averageFps || !averageLatency) return NVML_ERROR_INVALID_ARGUMENT;
    *sessionCount = 0;
    *averageFps = 0;
    *averageLatency = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetEncoderSessions(nvmlDevice_t device, unsigned int* sessionCount, void* sessionInfos) {
    if (!device || !sessionCount) return NVML_ERROR_INVALID_ARGUMENT;
    *sessionCount = 0;
    return NVML_SUCCESS;
}

/* FBC (Frame Buffer Capture) */
nvmlReturn_t nvmlDeviceGetFBCStats(nvmlDevice_t device, void* fbcStats) {
    (void)device; (void)fbcStats;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetFBCSessions(nvmlDevice_t device, unsigned int* sessionCount, void* sessionInfo) {
    if (!device || !sessionCount) return NVML_ERROR_INVALID_ARGUMENT;
    *sessionCount = 0;
    return NVML_SUCCESS;
}

/* Retired Pages */
nvmlReturn_t nvmlDeviceGetRetiredPages(nvmlDevice_t device, unsigned int cause, unsigned int* pageCount, unsigned long long* addresses) {
    if (!device || !pageCount) return NVML_ERROR_INVALID_ARGUMENT;
    *pageCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetRetiredPagesPendingStatus(nvmlDevice_t device, unsigned int* isPending) {
    if (!device || !isPending) return NVML_ERROR_INVALID_ARGUMENT;
    *isPending = 0;
    return NVML_SUCCESS;
}

/* ECC Errors */
nvmlReturn_t nvmlDeviceGetTotalEccErrors(nvmlDevice_t device, unsigned int errorType, unsigned int counterType, unsigned long long* eccCounts) {
    if (!device || !eccCounts) return NVML_ERROR_INVALID_ARGUMENT;
    *eccCounts = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMemoryErrorCounter(nvmlDevice_t device, unsigned int errorType, unsigned int counterType, unsigned int locationType, unsigned long long* count) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceClearEccErrorCounts(nvmlDevice_t device, unsigned int counterType) {
    (void)device; (void)counterType;
    return NVML_SUCCESS;
}

/* GPU Operation Mode */
nvmlReturn_t nvmlDeviceGetGpuOperationMode(nvmlDevice_t device, unsigned int* current, unsigned int* pending) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (current) *current = 0; /* All-on */
    if (pending) *pending = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetGpuOperationMode(nvmlDevice_t device, unsigned int mode) {
    (void)device; (void)mode;
    return NVML_SUCCESS;
}

/* Inforom */
nvmlReturn_t nvmlDeviceGetInforomImageVersion(nvmlDevice_t device, char* version, unsigned int length) {
    if (!device || !version) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(version, "G580.0200.00.02", length);
    version[length-1] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceValidateInforom(nvmlDevice_t device) {
    (void)device;
    return NVML_SUCCESS;
}

/* Attributes v2 */
typedef struct {
    unsigned int version;
    unsigned int multiprocessorCount;
    unsigned int sharedCopyEngineCount;
    unsigned int sharedDecoderCount;
    unsigned int sharedEncoderCount;
    unsigned int sharedJpegCount;
    unsigned int sharedOfaCount;
    unsigned int gpuInstanceSliceCount;
    unsigned int computeInstanceSliceCount;
    unsigned int memorySizeMB;
} nvmlDeviceAttributes_v2_t;

nvmlReturn_t nvmlDeviceGetAttributes_v2(nvmlDevice_t device, nvmlDeviceAttributes_v2_t* attributes) {
    if (!device || !attributes) return NVML_ERROR_INVALID_ARGUMENT;
    attributes->multiprocessorCount = 192; /* Blackwell-like */
    attributes->sharedCopyEngineCount = 2;
    attributes->sharedDecoderCount = 8;
    attributes->sharedEncoderCount = 8;
    attributes->sharedJpegCount = 8;
    attributes->sharedOfaCount = 1;
    attributes->gpuInstanceSliceCount = 0;
    attributes->computeInstanceSliceCount = 0;
    attributes->memorySizeMB = (unsigned int)(GPU_MEMORY_TOTAL / (1024 * 1024));
    return NVML_SUCCESS;
}

/* Virtualization Mode */
nvmlReturn_t nvmlDeviceGetVirtualizationMode(nvmlDevice_t device, unsigned int* pVirtualMode) {
    if (!device || !pVirtualMode) return NVML_ERROR_INVALID_ARGUMENT;
    *pVirtualMode = 0; /* None */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetVirtualizationMode(nvmlDevice_t device, unsigned int virtualMode) {
    (void)device; (void)virtualMode;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHostVgpuMode(nvmlDevice_t device, unsigned int* pHostVgpuMode) {
    if (!device || !pHostVgpuMode) return NVML_ERROR_INVALID_ARGUMENT;
    *pHostVgpuMode = 0;
    return NVML_SUCCESS;
}

/* vGPU functions */
nvmlReturn_t nvmlDeviceGetActiveVgpus(nvmlDevice_t device, unsigned int* vgpuCount, void* vgpuTypeIds) {
    if (!device || !vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    *vgpuCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCreatableVgpus(nvmlDevice_t device, unsigned int* vgpuCount, void* vgpuTypeIds) {
    if (!device || !vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    *vgpuCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetVgpuMetadata(nvmlDevice_t device, void* pgpuMetadata, unsigned int* bufferSize) {
    if (!device || !bufferSize) return NVML_ERROR_INVALID_ARGUMENT;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetVgpuCapabilities(nvmlDevice_t device, unsigned int capability, unsigned int* capResult) {
    if (!device || !capResult) return NVML_ERROR_INVALID_ARGUMENT;
    *capResult = 0;
    return NVML_SUCCESS;
}

/* Bridge Chip Info */
typedef struct {
    unsigned int type;
    unsigned int fwVersion;
} nvmlBridgeChipInfo_t;

typedef struct {
    unsigned int bridgeCount;
    nvmlBridgeChipInfo_t bridgeChipInfo[128];
} nvmlBridgeChipHierarchy_t;

nvmlReturn_t nvmlDeviceGetBridgeChipInfo(nvmlDevice_t device, nvmlBridgeChipHierarchy_t* bridgeHierarchy) {
    if (!device || !bridgeHierarchy) return NVML_ERROR_INVALID_ARGUMENT;
    bridgeHierarchy->bridgeCount = 0;
    return NVML_SUCCESS;
}

/* Compute Mode Setting */
nvmlReturn_t nvmlDeviceSetComputeMode(nvmlDevice_t device, nvmlComputeMode_t mode) {
    (void)device; (void)mode;
    return NVML_SUCCESS;
}

/* Persistence Mode Setting */
nvmlReturn_t nvmlDeviceSetPersistenceMode(nvmlDevice_t device, nvmlEnableState_t mode) {
    (void)device; (void)mode;
    return NVML_SUCCESS;
}

/* ECC Mode Setting */
nvmlReturn_t nvmlDeviceSetEccMode(nvmlDevice_t device, nvmlEnableState_t ecc) {
    (void)device; (void)ecc;
    return NVML_SUCCESS;
}

/* Power Management Limit Setting */
nvmlReturn_t nvmlDeviceSetPowerManagementLimit(nvmlDevice_t device, unsigned int limit) {
    (void)device; (void)limit;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetPowerManagementLimit_v2(nvmlDevice_t device, void* limitInfo) {
    (void)device; (void)limitInfo;
    return NVML_SUCCESS;
}

/* GSP Firmware */
nvmlReturn_t nvmlDeviceGetGspFirmwareVersion(nvmlDevice_t device, char* version) {
    if (!device || !version) return NVML_ERROR_INVALID_ARGUMENT;
    strcpy(version, "580.105.08");
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGspFirmwareMode(nvmlDevice_t device, unsigned int* isEnabled, unsigned int* defaultMode) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (isEnabled) *isEnabled = 1;
    if (defaultMode) *defaultMode = 1;
    return NVML_SUCCESS;
}

/* Driver Model v2 (Windows-specific) */
nvmlReturn_t nvmlDeviceGetDriverModel_v2(nvmlDevice_t device, unsigned int* current, unsigned int* pending) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceSetDriverModel(nvmlDevice_t device, unsigned int driverModel, unsigned int flags) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* CPU Affinity */
nvmlReturn_t nvmlDeviceGetCpuAffinity(nvmlDevice_t device, unsigned int cpuSetSize, unsigned long* cpuSet) {
    if (!device || !cpuSet || cpuSetSize == 0) return NVML_ERROR_INVALID_ARGUMENT;
    cpuSet[0] = 0xFFFFFFFF; /* All CPUs */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMemoryAffinity(nvmlDevice_t device, unsigned int nodeSetSize, unsigned long* nodeSet, unsigned int scope) {
    if (!device || !nodeSet || nodeSetSize == 0) return NVML_ERROR_INVALID_ARGUMENT;
    nodeSet[0] = 1; /* NUMA node 0 */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetNumaNodeId(nvmlDevice_t device, int* numaNodeId) {
    if (!device || !numaNodeId) return NVML_ERROR_INVALID_ARGUMENT;
    *numaNodeId = 0;
    return NVML_SUCCESS;
}

/* Topology */
nvmlReturn_t nvmlDeviceGetTopologyCommonAncestor(nvmlDevice_t device1, nvmlDevice_t device2, unsigned int* pathInfo) {
    if (!device1 || !device2 || !pathInfo) return NVML_ERROR_INVALID_ARGUMENT;
    *pathInfo = 5; /* Single system */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetTopologyNearestGpus(nvmlDevice_t device, unsigned int level, unsigned int* count, nvmlDevice_t* deviceArray) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetTopologyGpuSet(unsigned int cpuNumber, unsigned int* count, nvmlDevice_t* deviceArray) {
    if (!count) return NVML_ERROR_INVALID_ARGUMENT;
    if (*count == 0) {
        *count = 1;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    *count = 1;
    if (deviceArray) deviceArray[0] = &mock_device;
    return NVML_SUCCESS;
}

/* P2P Status */
nvmlReturn_t nvmlDeviceGetP2PStatus(nvmlDevice_t device1, nvmlDevice_t device2, unsigned int p2pIndex, unsigned int* p2pStatus) {
    if (!device1 || !device2 || !p2pStatus) return NVML_ERROR_INVALID_ARGUMENT;
    *p2pStatus = 0; /* Not supported */
    return NVML_SUCCESS;
}

/* NVLink */
nvmlReturn_t nvmlDeviceGetNvLinkCapability(nvmlDevice_t device, unsigned int link, unsigned int capability, unsigned int* capResult) {
    if (!device || !capResult) return NVML_ERROR_INVALID_ARGUMENT;
    *capResult = 0;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetNvLinkRemotePciInfo_v2(nvmlDevice_t device, unsigned int link, nvmlPciInfo_t* pci) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceResetNvLinkErrorCounters(nvmlDevice_t device, unsigned int link) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetNvLinkInfo(nvmlDevice_t device, unsigned int link, void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetNvlinkBwMode(nvmlDevice_t device, unsigned int* mode) {
    if (!device || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceSetNvlinkBwMode(nvmlDevice_t device, unsigned int mode) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceSetNvLinkDeviceLowPowerThreshold(nvmlDevice_t device, void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetNvlinkSupportedBwModes(nvmlDevice_t device, void* modes) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemGetNvlinkBwMode(unsigned int* mode) {
    if (!mode) return NVML_ERROR_INVALID_ARGUMENT;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemSetNvlinkBwMode(unsigned int mode) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Field Values */
nvmlReturn_t nvmlDeviceGetFieldValues(nvmlDevice_t device, int valuesCount, void* values) {
    (void)device; (void)valuesCount; (void)values;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceClearFieldValues(nvmlDevice_t device, int valuesCount, void* values) {
    (void)device; (void)valuesCount; (void)values;
    return NVML_SUCCESS;
}

/* Events */
nvmlReturn_t nvmlEventSetCreate(void** set) {
    if (!set) return NVML_ERROR_INVALID_ARGUMENT;
    *set = (void*)1; /* Dummy handle */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlEventSetFree(void* set) {
    (void)set;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlEventSetWait_v2(void* set, void* data, unsigned int timeoutMs) {
    (void)set; (void)data; (void)timeoutMs;
    return NVML_ERROR_TIMEOUT;
}

nvmlReturn_t nvmlDeviceRegisterEvents(nvmlDevice_t device, unsigned long long eventTypes, void* set) {
    (void)device; (void)eventTypes; (void)set;
    return NVML_SUCCESS;
}

/* Violation Status */
nvmlReturn_t nvmlDeviceGetViolationStatus(nvmlDevice_t device, unsigned int perfPolicyType, void* violTime) {
    (void)device; (void)perfPolicyType; (void)violTime;
    return NVML_ERROR_NOT_SUPPORTED;
}

/* System Info */
nvmlReturn_t nvmlSystemGetProcessName(unsigned int pid, char* name, unsigned int length) {
    if (!name || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(name, "unknown", length);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetHicVersion(unsigned int* hwbcCount, void* hwbcEntries) {
    if (!hwbcCount) return NVML_ERROR_INVALID_ARGUMENT;
    *hwbcCount = 0;
    return NVML_SUCCESS;
}

/* Unit functions (for S-class systems) */
nvmlReturn_t nvmlUnitGetCount(unsigned int* unitCount) {
    if (!unitCount) return NVML_ERROR_INVALID_ARGUMENT;
    *unitCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlUnitGetHandleByIndex(unsigned int index, void** unit) {
    return NVML_ERROR_NOT_FOUND;
}

nvmlReturn_t nvmlUnitGetUnitInfo(void* unit, void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlUnitGetLedState(void* unit, void* state) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlUnitSetLedState(void* unit, unsigned int color) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlUnitGetPsuInfo(void* unit, void* psu) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlUnitGetTemperature(void* unit, unsigned int type, unsigned int* temp) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlUnitGetFanSpeedInfo(void* unit, void* fanSpeeds) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlUnitGetDevices(void* unit, unsigned int* deviceCount, nvmlDevice_t* devices) {
    if (!deviceCount) return NVML_ERROR_INVALID_ARGUMENT;
    *deviceCount = 0;
    return NVML_SUCCESS;
}

/* MIG functions */
nvmlReturn_t nvmlDeviceGetMaxMigDeviceCount(nvmlDevice_t device, unsigned int* count) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMigDeviceHandleByIndex(nvmlDevice_t device, unsigned int index, nvmlDevice_t* migDevice) {
    return NVML_ERROR_NOT_FOUND;
}

nvmlReturn_t nvmlDeviceIsMigDeviceHandle(nvmlDevice_t device, unsigned int* isMigDevice) {
    if (!device || !isMigDevice) return NVML_ERROR_INVALID_ARGUMENT;
    *isMigDevice = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGpuInstanceId(nvmlDevice_t device, unsigned int* id) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetComputeInstanceId(nvmlDevice_t device, unsigned int* id) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceSetMigMode(nvmlDevice_t device, unsigned int mode, unsigned int* activationStatus) {
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetGpuInstances(nvmlDevice_t device, unsigned int profileId, void* gpuInstances, unsigned int* count) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGpuInstanceRemainingCapacity(nvmlDevice_t device, unsigned int profileId, unsigned int* count) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceCreateGpuInstance(nvmlDevice_t device, unsigned int profileId, void** gpuInstance) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceCreateGpuInstanceWithPlacement(nvmlDevice_t device, unsigned int profileId, void* placement, void** gpuInstance) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetGpuInstancePossiblePlacements_v2(nvmlDevice_t device, unsigned int profileId, void* placements, unsigned int* count) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGpuInstanceProfileInfoV(nvmlDevice_t device, unsigned int profile, void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetDeviceHandleFromMigDeviceHandle(nvmlDevice_t migDevice, nvmlDevice_t* device) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceDestroy(void* gpuInstance) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceGetInfo(void* gpuInstance, void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceGetComputeInstances(void* gpuInstance, unsigned int profileId, void* computeInstances, unsigned int* count) {
    if (!count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpuInstanceGetComputeInstanceRemainingCapacity(void* gpuInstance, unsigned int profileId, unsigned int* count) {
    if (!count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpuInstanceCreateComputeInstance(void* gpuInstance, unsigned int profileId, void** computeInstance) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceCreateComputeInstanceWithPlacement(void* gpuInstance, unsigned int profileId, void* placement, void** computeInstance) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceGetComputeInstanceProfileInfoV(void* gpuInstance, unsigned int profile, unsigned int engProfile, void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceGetComputeInstancePossiblePlacements(void* gpuInstance, unsigned int profileId, void* placements, unsigned int* count) {
    if (!count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlComputeInstanceDestroy(void* computeInstance) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlComputeInstanceGetInfo_v2(void* computeInstance, void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Temperature thresholds */
nvmlReturn_t nvmlDeviceGetTemperatureThreshold(nvmlDevice_t device, unsigned int thresholdType, unsigned int* temp) {
    if (!device || !temp) return NVML_ERROR_INVALID_ARGUMENT;
    *temp = 83; /* Typical throttle temp */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetTemperatureThreshold(nvmlDevice_t device, unsigned int thresholdType, int* temp) {
    (void)device; (void)thresholdType; (void)temp;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetTemperatureV(nvmlDevice_t device, unsigned int tempType, unsigned int* temp) {
    if (!device || !temp) return NVML_ERROR_INVALID_ARGUMENT;
    *temp = GPU_TEMPERATURE;
    return NVML_SUCCESS;
}

/* Drain state (for hot-plug support) */
nvmlReturn_t nvmlDeviceModifyDrainState(nvmlPciInfo_t* pciInfo, unsigned int newState) {
    (void)pciInfo; (void)newState;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceQueryDrainState(nvmlPciInfo_t* pciInfo, unsigned int* currentState) {
    if (!pciInfo || !currentState) return NVML_ERROR_INVALID_ARGUMENT;
    *currentState = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceRemoveGpu_v2(nvmlPciInfo_t* pciInfo, unsigned int gpuState, unsigned int linkState) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceDiscoverGpus(nvmlPciInfo_t* pciInfo) {
    return NVML_SUCCESS;
}

/* Excluded devices */
nvmlReturn_t nvmlGetExcludedDeviceCount(unsigned int* deviceCount) {
    if (!deviceCount) return NVML_ERROR_INVALID_ARGUMENT;
    *deviceCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGetExcludedDeviceInfoByIndex(unsigned int index, void* info) {
    return NVML_ERROR_NOT_FOUND;
}

/* GPU locked clocks */
nvmlReturn_t nvmlDeviceSetGpuLockedClocks(nvmlDevice_t device, unsigned int minGpuClockMHz, unsigned int maxGpuClockMHz) {
    (void)device; (void)minGpuClockMHz; (void)maxGpuClockMHz;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceResetGpuLockedClocks(nvmlDevice_t device) {
    (void)device;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetMemoryLockedClocks(nvmlDevice_t device, unsigned int minMemClockMHz, unsigned int maxMemClockMHz) {
    (void)device; (void)minMemClockMHz; (void)maxMemClockMHz;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceResetMemoryLockedClocks(nvmlDevice_t device) {
    (void)device;
    return NVML_SUCCESS;
}

/* API Restriction */
nvmlReturn_t nvmlDeviceSetAPIRestriction(nvmlDevice_t device, unsigned int apiType, nvmlEnableState_t isRestricted) {
    (void)device; (void)apiType; (void)isRestricted;
    return NVML_SUCCESS;
}

/* Compute mode getter/setter already defined above */

/* Additional utilization metrics */
nvmlReturn_t nvmlDeviceGetJpgUtilization(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs) {
    if (!device || !utilization || !samplingPeriodUs) return NVML_ERROR_INVALID_ARGUMENT;
    *utilization = 0;
    *samplingPeriodUs = 100000;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetOfaUtilization(nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs) {
    if (!device || !utilization || !samplingPeriodUs) return NVML_ERROR_INVALID_ARGUMENT;
    *utilization = 0;
    *samplingPeriodUs = 100000;
    return NVML_SUCCESS;
}

/* Capabilities */
nvmlReturn_t nvmlDeviceGetCapabilities(nvmlDevice_t device, unsigned int capability, unsigned int* capResult) {
    if (!device || !capResult) return NVML_ERROR_INVALID_ARGUMENT;
    *capResult = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGetVgpuDriverCapabilities(unsigned int capability, unsigned int* capResult) {
    if (!capResult) return NVML_ERROR_INVALID_ARGUMENT;
    *capResult = 0;
    return NVML_SUCCESS;
}

/* Addressing Mode */
nvmlReturn_t nvmlDeviceGetAddressingMode(nvmlDevice_t device, unsigned int* mode) {
    if (!device || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = 0; /* Default */
    return NVML_SUCCESS;
}

/* DRAM Encryption */
nvmlReturn_t nvmlDeviceGetDramEncryptionMode(nvmlDevice_t device, unsigned int* isEnabled) {
    if (!device || !isEnabled) return NVML_ERROR_INVALID_ARGUMENT;
    *isEnabled = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetDramEncryptionMode(nvmlDevice_t device, unsigned int mode) {
    (void)device; (void)mode;
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Confidential Compute */
nvmlReturn_t nvmlDeviceGetConfComputeMemSizeInfo(nvmlDevice_t device, void* memInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetConfComputeProtectedMemoryUsage(nvmlDevice_t device, void* memory) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceSetConfComputeUnprotectedMemSize(nvmlDevice_t device, unsigned long long sizeKiB) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemGetConfComputeCapabilities(void* capabilities) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemGetConfComputeState(void* state) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemGetConfComputeSettings(void* settings) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemGetConfComputeGpusReadyState(unsigned int* isReady) {
    if (!isReady) return NVML_ERROR_INVALID_ARGUMENT;
    *isReady = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemSetConfComputeGpusReadyState(unsigned int isReady) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemGetConfComputeKeyRotationThresholdInfo(void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemSetConfComputeKeyRotationThresholdInfo(void* info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* C2C Mode */
nvmlReturn_t nvmlDeviceGetC2cModeInfoV(nvmlDevice_t device, void* c2cModeInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* GPU Fabric */
nvmlReturn_t nvmlDeviceGetGpuFabricInfoV(nvmlDevice_t device, void* gpuFabricInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Module ID */
nvmlReturn_t nvmlDeviceGetModuleId(nvmlDevice_t device, unsigned int* moduleId) {
    if (!device || !moduleId) return NVML_ERROR_INVALID_ARGUMENT;
    *moduleId = 0;
    return NVML_SUCCESS;
}

/* Repair Status */
nvmlReturn_t nvmlDeviceGetRepairStatus(nvmlDevice_t device, unsigned int* status) {
    if (!device || !status) return NVML_ERROR_INVALID_ARGUMENT;
    *status = 0;
    return NVML_SUCCESS;
}

/* GPC Clock VF Offset */
nvmlReturn_t nvmlDeviceGetGpcClkVfOffset(nvmlDevice_t device, int* offset) {
    if (!device || !offset) return NVML_ERROR_INVALID_ARGUMENT;
    *offset = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetGpcClkVfOffset(nvmlDevice_t device, int offset) {
    (void)device; (void)offset;
    return NVML_SUCCESS;
}

/* Margin Temperature */
nvmlReturn_t nvmlDeviceGetMarginTemperature(nvmlDevice_t device, unsigned int marginType, int* tempDelta) {
    if (!device || !tempDelta) return NVML_ERROR_INVALID_ARGUMENT;
    *tempDelta = 38; /* 83 - 45 = 38C margin */
    return NVML_SUCCESS;
}

/* Last BBX Flush Time */
nvmlReturn_t nvmlDeviceGetLastBBXFlushTime(nvmlDevice_t device, unsigned long long* timestamp, unsigned long long* duration) {
    if (!device || !timestamp || !duration) return NVML_ERROR_INVALID_ARGUMENT;
    *timestamp = 0;
    *duration = 0;
    return NVML_SUCCESS;
}

/* PDI (Part Device ID) */
nvmlReturn_t nvmlDeviceGetPdi(nvmlDevice_t device, char* pdi, unsigned int length) {
    if (!device || !pdi) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(pdi, "N/A", length);
    return NVML_SUCCESS;
}

/* Platform Info */
nvmlReturn_t nvmlDeviceGetPlatformInfo(nvmlDevice_t device, void* platformInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* PCI Info Extended */
nvmlReturn_t nvmlDeviceGetPciInfoExt(nvmlDevice_t device, void* pciInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Hostname */
nvmlReturn_t nvmlDeviceGetHostname_v1(nvmlDevice_t device, char* hostname, unsigned int length) {
    if (!device || !hostname) return NVML_ERROR_INVALID_ARGUMENT;
    strncpy(hostname, "localhost", length);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetHostname_v1(nvmlDevice_t device, const char* hostname) {
    (void)device; (void)hostname;
    return NVML_SUCCESS;
}

/* Grid License */
nvmlReturn_t nvmlDeviceGetGridLicensableFeatures_v4(nvmlDevice_t device, void* pGridLicensableFeatures) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* vGPU Scheduler */
nvmlReturn_t nvmlDeviceGetVgpuSchedulerLog(nvmlDevice_t device, void* pSchedulerLog) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetVgpuSchedulerState(nvmlDevice_t device, void* pSchedulerState) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetVgpuSchedulerCapabilities(nvmlDevice_t device, void* pCapabilities) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceSetVgpuSchedulerState(nvmlDevice_t device, void* pSchedulerState) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetVgpuHeterogeneousMode(nvmlDevice_t device, unsigned int* mode) {
    if (!device || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetVgpuHeterogeneousMode(nvmlDevice_t device, unsigned int mode) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetVgpuTypeSupportedPlacements(nvmlDevice_t device, unsigned int vgpuTypeId, void* pPlacements, unsigned int* count) {
    if (!count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetVgpuTypeCreatablePlacements(nvmlDevice_t device, unsigned int vgpuTypeId, void* pPlacements, unsigned int* count) {
    if (!count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceSetVgpuCapabilities(nvmlDevice_t device, unsigned int capability, unsigned int state) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetVgpuProcessesUtilizationInfo(nvmlDevice_t device, void* pVgpuProcessesUtilizationInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetVgpuInstancesUtilizationInfo(nvmlDevice_t device, void* pVgpuUtilInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* vGPU Instance functions */
nvmlReturn_t nvmlVgpuInstanceGetAccountingMode(unsigned int vgpuInstance, unsigned int* mode) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetAccountingPids(unsigned int vgpuInstance, unsigned int* count, unsigned int* pids) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetAccountingStats(unsigned int vgpuInstance, unsigned int pid, void* stats) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceClearAccountingPids(unsigned int vgpuInstance) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetEccMode(unsigned int vgpuInstance, unsigned int* eccMode) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetEncoderStats(unsigned int vgpuInstance, unsigned int* sessionCount, unsigned int* averageFps, unsigned int* averageLatency) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetEncoderSessions(unsigned int vgpuInstance, unsigned int* sessionCount, void* sessionInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetFBCStats(unsigned int vgpuInstance, void* fbcStats) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetFBCSessions(unsigned int vgpuInstance, unsigned int* sessionCount, void* sessionInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* GPM (GPU Performance Monitoring) */
nvmlReturn_t nvmlGpmSampleAlloc(void** gpmSample) {
    if (!gpmSample) return NVML_ERROR_INVALID_ARGUMENT;
    *gpmSample = (void*)1;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpmSampleFree(void* gpmSample) {
    (void)gpmSample;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpmSampleGet(nvmlDevice_t device, void* gpmSample) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpmMetricsGet(void* metricsGet) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpmQueryIfStreamingEnabled(nvmlDevice_t device, unsigned int* state) {
    if (!device || !state) return NVML_ERROR_INVALID_ARGUMENT;
    *state = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpmSetStreamingEnabled(nvmlDevice_t device, unsigned int state) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* MIG-related GPU instance functions */
nvmlReturn_t nvmlGpuInstanceGetActiveVgpus(void* gpuInstance, unsigned int* vgpuCount, void* vgpuTypeIds) {
    if (!vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    *vgpuCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpuInstanceGetCreatableVgpus(void* gpuInstance, unsigned int* vgpuCount, void* vgpuTypeIds) {
    if (!vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    *vgpuCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpuInstanceGetVgpuHeterogeneousMode(void* gpuInstance, unsigned int* mode) {
    if (!mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlGpuInstanceSetVgpuHeterogeneousMode(void* gpuInstance, unsigned int mode) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceGetVgpuSchedulerLog(void* gpuInstance, void* pSchedulerLog) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceGetVgpuSchedulerState(void* gpuInstance, void* pSchedulerState) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceSetVgpuSchedulerState(void* gpuInstance, void* pSchedulerState) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlGpuInstanceGetVgpuTypeCreatablePlacements(void* gpuInstance, unsigned int vgpuTypeId, void* pPlacements, unsigned int* count) {
    if (!count) return NVML_ERROR_INVALID_ARGUMENT;
    *count = 0;
    return NVML_SUCCESS;
}

/* PRM (Performance Register Monitor) */
nvmlReturn_t nvmlDeviceReadPRMCounters_v1(nvmlDevice_t device, void* counters) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceReadWritePRM_v1(nvmlDevice_t device, void* params) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Workload Power Profile */
nvmlReturn_t nvmlDeviceWorkloadPowerProfileGetProfilesInfo(nvmlDevice_t device, void* profilesInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceWorkloadPowerProfileGetCurrentProfiles(nvmlDevice_t device, void* profiles) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceWorkloadPowerProfileSetRequestedProfiles(nvmlDevice_t device, void* profiles) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceWorkloadPowerProfileClearRequestedProfiles(nvmlDevice_t device) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Power Smoothing */
nvmlReturn_t nvmlDevicePowerSmoothingSetState(nvmlDevice_t device, void* state) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDevicePowerSmoothingActivatePresetProfile(nvmlDevice_t device, unsigned int profileId) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDevicePowerSmoothingUpdatePresetProfileParam(nvmlDevice_t device, void* params) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* === Remaining functions for nvidia-smi === */

/* Retired Pages v2 */
nvmlReturn_t nvmlDeviceGetRetiredPages_v2(nvmlDevice_t device, unsigned int cause, unsigned int* pageCount, unsigned long long* addresses, unsigned long long* timestamps) {
    if (!device || !pageCount) return NVML_ERROR_INVALID_ARGUMENT;
    *pageCount = 0;
    return NVML_SUCCESS;
}

/* Row Remapper Histogram */
nvmlReturn_t nvmlDeviceGetRowRemapperHistogram(nvmlDevice_t device, void* values) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Samples (for historical data) */
nvmlReturn_t nvmlDeviceGetSamples(nvmlDevice_t device, unsigned int type, unsigned long long lastSeenTimeStamp, unsigned int* sampleValType, unsigned int* sampleCount, void* samples) {
    if (!device || !sampleValType || !sampleCount) return NVML_ERROR_INVALID_ARGUMENT;
    *sampleCount = 0;
    return NVML_SUCCESS;
}

/* SRAM ECC Error Status */
nvmlReturn_t nvmlDeviceGetSramEccErrorStatus(nvmlDevice_t device, void* status) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* Supported Event Types */
nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t device, unsigned long long* eventTypes) {
    if (!device || !eventTypes) return NVML_ERROR_INVALID_ARGUMENT;
    *eventTypes = 0;
    return NVML_SUCCESS;
}

/* Supported Clocks */
nvmlReturn_t nvmlDeviceGetSupportedGraphicsClocks(nvmlDevice_t device, unsigned int memoryClockMHz, unsigned int* count, unsigned int* clocksMHz) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    if (*count == 0) {
        *count = 1;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    *count = 1;
    if (clocksMHz) clocksMHz[0] = GPU_CLOCK_GRAPHICS;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetSupportedMemoryClocks(nvmlDevice_t device, unsigned int* count, unsigned int* clocksMHz) {
    if (!device || !count) return NVML_ERROR_INVALID_ARGUMENT;
    if (*count == 0) {
        *count = 1;
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    *count = 1;
    if (clocksMHz) clocksMHz[0] = GPU_CLOCK_MEMORY;
    return NVML_SUCCESS;
}

/* Supported vGPUs */
nvmlReturn_t nvmlDeviceGetSupportedVgpus(nvmlDevice_t device, unsigned int* vgpuCount, void* vgpuTypeIds) {
    if (!device || !vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    *vgpuCount = 0;
    return NVML_SUCCESS;
}

/* vGPU Instance functions */
nvmlReturn_t nvmlVgpuInstanceGetFbUsage(unsigned int vgpuInstance, unsigned long long* fbUsage) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetFrameRateLimit(unsigned int vgpuInstance, unsigned int* frameRateLimit) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetGpuInstanceId(unsigned int vgpuInstance, unsigned int* gpuInstanceId) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetGpuPciId(unsigned int vgpuInstance, char* vgpuPciId, unsigned int* length) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetLicenseInfo_v2(unsigned int vgpuInstance, void* licenseInfo) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetMdevUUID(unsigned int vgpuInstance, char* mdevUuid, unsigned int size) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetPlacementId(unsigned int vgpuInstance, void* placementId) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetType(unsigned int vgpuInstance, unsigned int* vgpuTypeId) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetUUID(unsigned int vgpuInstance, char* uuid, unsigned int size) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetVmDriverVersion(unsigned int vgpuInstance, char* version, unsigned int length) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuInstanceGetVmID(unsigned int vgpuInstance, char* vmId, unsigned int size, unsigned int* vmIdType) {
    return NVML_ERROR_NOT_SUPPORTED;
}

/* vGPU Type functions */
nvmlReturn_t nvmlVgpuTypeGetBAR1Info(unsigned int vgpuTypeId, void* bar1Info) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetCapabilities(unsigned int vgpuTypeId, unsigned int capability, unsigned int* capResult) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetClass(unsigned int vgpuTypeId, char* vgpuTypeClass, unsigned int* size) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetDeviceID(unsigned int vgpuTypeId, unsigned long long* deviceId, unsigned long long* subsystemId) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetFramebufferSize(unsigned int vgpuTypeId, unsigned long long* fbSize) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetFrameRateLimit(unsigned int vgpuTypeId, unsigned int* frameRateLimit) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetGpuInstanceProfileId(unsigned int vgpuTypeId, unsigned int* gpuInstanceProfileId) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetLicense(unsigned int vgpuTypeId, char* vgpuTypeLicenseString, unsigned int size) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetMaxInstances(nvmlDevice_t device, unsigned int vgpuTypeId, unsigned int* vgpuInstanceCount) {
    if (!device || !vgpuInstanceCount) return NVML_ERROR_INVALID_ARGUMENT;
    *vgpuInstanceCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetMaxInstancesPerGpuInstance(unsigned int vgpuTypeId, void* gpuInstanceProfileInfo, unsigned int* maxInstances) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetMaxInstancesPerVm(unsigned int vgpuTypeId, unsigned int* maxInstances) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetName(unsigned int vgpuTypeId, char* vgpuTypeName, unsigned int* size) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetNumDisplayHeads(unsigned int vgpuTypeId, unsigned int* numDisplayHeads) {
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlVgpuTypeGetResolution(unsigned int vgpuTypeId, unsigned int displayIndex, unsigned int* xdim, unsigned int* ydim) {
    return NVML_ERROR_NOT_SUPPORTED;
}
