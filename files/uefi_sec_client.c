#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include "QSEEComAPI.h"

/* ===================================================================
   CONFIGURATION
   =================================================================== */
#define TA_APP_NAME "qcom.tz.uefisecapp"
#define TA_APP_PATH "/firmware/image"
#define MAX_BUFFER_SIZE 4096

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* Status Codes */
#define EFI_SUCCESS               0x00000000
#define EFI_BUFFER_TOO_SMALL      0x80000005
#define EFI_NOT_FOUND             0x8000000E

/* Command IDs */
#define CMD_UEFI_GET_VARIABLE     0x00008000
#define CMD_UEFI_SET_VARIABLE     0x00008001

/* GUIDs */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

EFI_GUID EFI_GLOBAL_VARIABLE = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C }};
EFI_GUID EFI_DB_VARIABLE     = { 0xd719b2cb, 0x3d3a, 0x4596, { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f }}; 

/* TA Request/Response Structures */
typedef struct {
    uint32_t CommandId;
    uint32_t Length;
    uint32_t VariableNameOffset;
    uint32_t VariableNameSize;
    uint32_t VendorGuidOffset;
    uint32_t VendorGuidSize;
    uint32_t Attributes;
    uint32_t DataSize; 
} TZ_UEFI_GET_REQ;

typedef struct {
    uint32_t CommandId;
    uint32_t Length;
    uint32_t Status;      
    uint32_t Attributes;
    uint32_t DataOffset;
    uint32_t DataSize;
} TZ_UEFI_GET_RSP;

typedef struct {
    uint32_t CommandId;
    uint32_t Length;
    uint32_t VariableNameOffset;
    uint32_t VariableNameSize;
    uint32_t VendorGuidOffset;
    uint32_t VendorGuidSize;
    uint32_t Attributes;
    uint32_t DataOffset;
    uint32_t DataSize;
} TZ_UEFI_SET_REQ;

typedef struct {
    uint32_t CommandId;
    uint32_t Length;
    uint32_t Status;
    uint32_t TableId;
    uint32_t SyncSize;
} TZ_UEFI_SET_RSP;

struct QSEECom_handle *l_handle = NULL;
struct QSEECom_ion_fd_info ion_fd_info;
pthread_mutex_t cmd_lock = PTHREAD_MUTEX_INITIALIZER;

/* ===================================================================
   TA COMMUNICATION FUNCTIONS
   =================================================================== */

/* 
 * Generic GET Command
 * If req_data_len == 0, we only want to check existence/size (Metadata).
 * If req_data_len > 0, we try to read that many bytes.
 */
int uefi_send_get(const char *name, uint32_t req_data_len, uint32_t *out_status, uint32_t *out_actual_size, uint8_t *buffer) {
    if (!l_handle) return -1;
    pthread_mutex_lock(&cmd_lock);

    memset(l_handle->ion_sbuffer, 0, MAX_BUFFER_SIZE);
    TZ_UEFI_GET_REQ *req = (TZ_UEFI_GET_REQ *)l_handle->ion_sbuffer;

    uint32_t name_len = (strlen(name) + 1) * 2;
    uint32_t off_name = sizeof(TZ_UEFI_GET_REQ);
    uint32_t off_guid = off_name + name_len;
    
    req->CommandId = CMD_UEFI_GET_VARIABLE;
    req->VariableNameOffset = off_name;
    req->VariableNameSize = name_len;
    req->VendorGuidOffset = off_guid;
    req->VendorGuidSize = sizeof(EFI_GUID);
    req->Attributes = 0; 
    req->DataSize = req_data_len; // 0 for check, 4 for boolean read
    
    // Name to Unicode
    uint8_t *ptr_name = (uint8_t *)l_handle->ion_sbuffer + off_name;
    for (int i = 0; i < strlen(name); i++) ptr_name[i*2] = name[i];

    // GUID Selection
    EFI_GUID *g = &EFI_GLOBAL_VARIABLE;
    if (strcmp(name, "db") == 0 || strcmp(name, "dbx") == 0) g = &EFI_DB_VARIABLE;
    memcpy((uint8_t *)l_handle->ion_sbuffer + off_guid, g, sizeof(EFI_GUID));

    req->Length = off_guid + sizeof(EFI_GUID);

    int ret = QSEECom_send_modified_cmd(l_handle, 
            l_handle->ion_sbuffer, ALIGN(req->Length, 64), 
            l_handle->ion_sbuffer, MAX_BUFFER_SIZE, &ion_fd_info);

    if (ret != 0) {
        pthread_mutex_unlock(&cmd_lock);
        return ret;
    }

    TZ_UEFI_GET_RSP *rsp = (TZ_UEFI_GET_RSP *)l_handle->ion_sbuffer;
    *out_status = rsp->Status;
    *out_actual_size = rsp->DataSize;

    // If we requested data and got Success (or BufferTooSmall but data returned), copy it
    if (buffer && req_data_len > 0 && rsp->DataSize > 0) {
        // Copy only what fits in request or response, whichever is smaller
        uint32_t copy_len = (rsp->DataSize < req_data_len) ? rsp->DataSize : req_data_len;
        memcpy(buffer, (uint8_t *)l_handle->ion_sbuffer + rsp->DataOffset, copy_len);
    }

    pthread_mutex_unlock(&cmd_lock);
    return 0;
}

/* 
 * Enroll (Set Variable)
 */
int uefi_enroll(const char *name, const char *file_path) {
    if (!l_handle) return -1;

    FILE *fp = fopen(file_path, "rb");
    if (!fp) { printf("[ERROR] File not found: %s\n", file_path); return -1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc(fsize);
    fread(buf, 1, fsize, fp);
    fclose(fp);
    
    pthread_mutex_lock(&cmd_lock);
    memset(l_handle->ion_sbuffer, 0, MAX_BUFFER_SIZE);
    TZ_UEFI_SET_REQ *req = (TZ_UEFI_SET_REQ *)l_handle->ion_sbuffer;

    uint32_t name_len = (strlen(name) + 1) * 2;
    uint32_t off_name = sizeof(TZ_UEFI_SET_REQ);
    uint32_t off_guid = off_name + name_len;
    uint32_t off_data = off_guid + sizeof(EFI_GUID);

    req->CommandId = CMD_UEFI_SET_VARIABLE;
    req->VariableNameOffset = off_name;
    req->VariableNameSize = name_len;
    req->VendorGuidOffset = off_guid;
    req->VendorGuidSize = sizeof(EFI_GUID);
    req->Attributes = 0x27; // NV | BS | RT | TIME_AUTH
    req->DataOffset = off_data;
    req->DataSize = (uint32_t)fsize;

    uint8_t *ptr_name = (uint8_t *)l_handle->ion_sbuffer + off_name;
    for (int i = 0; i < strlen(name); i++) ptr_name[i*2] = name[i];
    
    EFI_GUID *g = &EFI_GLOBAL_VARIABLE;
    if (strcmp(name, "db") == 0 || strcmp(name, "dbx") == 0) g = &EFI_DB_VARIABLE;
    memcpy((uint8_t *)l_handle->ion_sbuffer + off_guid, g, sizeof(EFI_GUID));

    if (off_data + fsize > MAX_BUFFER_SIZE) {
        printf("[ERROR] Buffer overflow\n");
        free(buf); pthread_mutex_unlock(&cmd_lock); return -1;
    }
    memcpy((uint8_t *)l_handle->ion_sbuffer + off_data, buf, fsize);
    req->Length = off_data + fsize;

    int ret = QSEECom_send_modified_cmd(l_handle, 
            l_handle->ion_sbuffer, ALIGN(req->Length, 64), 
            l_handle->ion_sbuffer, MAX_BUFFER_SIZE, &ion_fd_info);
    
    TZ_UEFI_SET_RSP *rsp = (TZ_UEFI_SET_RSP *)l_handle->ion_sbuffer;
    uint32_t status = rsp->Status;
    
    pthread_mutex_unlock(&cmd_lock);
    free(buf);

    if (ret == 0 && status == EFI_SUCCESS) {
        printf("[INFO] Enroll Success.\n");
    } else {
        printf("[ERROR] Enroll Failed. Status: 0x%X\n", status);
    }
    return status;
}

/* ===================================================================
   MAIN
   =================================================================== */
/* 
   Keep all previous includes, defines, structures, and helper functions (uefi_send_get, uefi_enroll).
   ONLY the main function is updated below.
*/

int main(int argc, char *argv[]) {
    int ret = QSEECom_start_app(&l_handle, TA_APP_PATH, TA_APP_NAME, MAX_BUFFER_SIZE);
    if (ret != 0) {
        printf("[CRITICAL] Failed to load TrustZone App: %d\n", ret);
        return -1;
    }

    if (argc < 2) {
        printf("Usage: %s -checkStatus | -enroll -var <PK|KEK|db|dbx> -esl <file.auth>\n", argv[0]);
        QSEECom_shutdown_app(&l_handle);
        return 0;
    }

    if (strcmp(argv[1], "-checkStatus") == 0) {
        printf("\n=== System Security State ===\n");
        
        uint32_t status, size;
        uint8_t val[8] = {0}; 

        // 1. SetupMode
        uefi_send_get("SetupMode", 4, &status, &size, val);
        if (status == EFI_SUCCESS || (status == EFI_BUFFER_TOO_SMALL && size > 0)) {
            printf(" [State] SetupMode    : %s\n", val[0] ? "1 (Setup - Unlocked)" : "0 (User - Locked)  <-- SUCCESS");
        } else {
            printf(" [State] SetupMode    : UNKNOWN (0x%X)\n", status);
        }

        // 2. SecureBoot
        memset(val, 0, 8);
        uefi_send_get("SecureBoot", 4, &status, &size, val);
        if (status == EFI_SUCCESS || (status == EFI_BUFFER_TOO_SMALL && size > 0)) {
            printf(" [State] SecureBoot   : %s\n", val[0] ? "ENABLED" : "DISABLED");
        } else {
            printf(" [State] SecureBoot   : UNKNOWN (0x%X)\n", status);
        }

        // 3. AuditMode (New)
        memset(val, 0, 8);
        uefi_send_get("AuditMode", 4, &status, &size, val);
        if (status == EFI_SUCCESS || (status == EFI_BUFFER_TOO_SMALL && size > 0)) {
            printf(" [State] AuditMode    : %d %s\n", val[0], val[0] ? "(Logging Only - No Enforce)" : "(Off)");
        }

        // 4. DeployedMode (New)
        memset(val, 0, 8);
        uefi_send_get("DeployedMode", 4, &status, &size, val);
        if (status == EFI_SUCCESS || (status == EFI_BUFFER_TOO_SMALL && size > 0)) {
            printf(" [State] DeployedMode : %d\n", val[0]);
        }

        printf("\n=== Key Database ===\n");
        const char *keys[] = {"PK", "KEK", "db", "dbx"};
        
        for (int i=0; i<4; i++) {
            uefi_send_get(keys[i], 0, &status, &size, NULL);
            printf(" [%3s] ", keys[i]);
            
            if (status == EFI_SUCCESS || status == EFI_BUFFER_TOO_SMALL) {
                if (size > 0) printf("Present (Size: %d)\n", size);
                else          printf("Not Provisioned (Empty)\n");
            }
            else if (status == EFI_NOT_FOUND) {
                printf("Not Provisioned\n");
            }
            else {
                printf("Error 0x%X\n", status);
            }
        }
        printf("===========================\n");
    }
    else if (strcmp(argv[1], "-enroll") == 0) {
        char *var = NULL;
        char *file = NULL;
        for (int i=2; i<argc; i++) {
            if (strcmp(argv[i], "-var") == 0) var = argv[++i];
            if (strcmp(argv[i], "-esl") == 0) file = argv[++i];
        }
        if (var && file) uefi_enroll(var, file);
        else printf("[ERROR] Usage: -enroll -var <Name> -esl <File>\n");
    }

    QSEECom_shutdown_app(&l_handle);
    return 0;
}

