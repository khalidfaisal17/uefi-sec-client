/*
 * Copyright (c) 2024 Qualcomm Technologies, Inc.
 * All Rights Reserved.
 * Confidential and Proprietary - Qualcomm Technologies, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include "QSEEComAPI.h"

/* ===================================================================
   CONFIGURATION
   =================================================================== */
#define TA_APP_NAME "qcom.tz.uefisecapp"
#define TA_APP_PATH "/firmware/image"
#define MAX_BUFFER_SIZE 8192  /* Increased for larger cert lists */
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* Status Codes */
#define EFI_SUCCESS               0x00000000
#define EFI_BUFFER_TOO_SMALL      0x80000005
#define EFI_NOT_FOUND             0x8000000E

/* Attributes */
#define EFI_VARIABLE_NON_VOLATILE                          0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS                    0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS                        0x00000004
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020

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

static const EFI_GUID EFI_GLOBAL_VARIABLE = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C }};
static const EFI_GUID EFI_DB_VARIABLE     = { 0xd719b2cb, 0x3d3a, 0x4596, { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f }}; 

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

int uefi_send_get(const char *name, uint32_t req_data_len, uint32_t *out_status, uint32_t *out_actual_size, uint8_t *buffer) {
    if (!l_handle) return -1;
    pthread_mutex_lock(&cmd_lock);
    memset(l_handle->ion_sbuffer, 0, MAX_BUFFER_SIZE);

    TZ_UEFI_GET_REQ *req = (TZ_UEFI_GET_REQ *)l_handle->ion_sbuffer;
    uint32_t name_len = (strlen(name) + 1) * 2;
    uint32_t off_name = sizeof(TZ_UEFI_GET_REQ);
    uint32_t off_guid = ALIGN(off_name + name_len, 8);

    req->CommandId = CMD_UEFI_GET_VARIABLE;
    req->VariableNameOffset = off_name;
    req->VariableNameSize = name_len;
    req->VendorGuidOffset = off_guid;
    req->VendorGuidSize = sizeof(EFI_GUID);
    req->Attributes = 0; 
    req->DataSize = req_data_len;

    /* Convert ASCII Name to Unicode (UCS-2) */
    uint8_t *ptr_name = (uint8_t *)l_handle->ion_sbuffer + off_name;
    for (int i = 0; i < strlen(name); i++) ptr_name[i*2] = name[i];

    /* Select GUID */
    const EFI_GUID *g = &EFI_GLOBAL_VARIABLE;
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

    if (buffer && req_data_len > 0 && rsp->DataSize > 0) {
        uint32_t copy_len = (rsp->DataSize < req_data_len) ? rsp->DataSize : req_data_len;
        memcpy(buffer, (uint8_t *)l_handle->ion_sbuffer + rsp->DataOffset, copy_len);
    }
    pthread_mutex_unlock(&cmd_lock);
    return 0;
}

int uefi_set_variable(const char *name, const char *file_path) {
    if (!l_handle) return -1;

    uint8_t *buf = NULL;
    long fsize = 0;

    if (file_path) {
        FILE *fp = fopen(file_path, "rb");
        if (!fp) { printf("[ERROR] File not found: %s\n", file_path); return -1; }
        fseek(fp, 0, SEEK_END);
        fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        buf = malloc(fsize);
        if (!buf) { fclose(fp); return -1; }
        fread(buf, 1, fsize, fp);
        fclose(fp);
    }

    pthread_mutex_lock(&cmd_lock);
    memset(l_handle->ion_sbuffer, 0, MAX_BUFFER_SIZE);

    TZ_UEFI_SET_REQ *req = (TZ_UEFI_SET_REQ *)l_handle->ion_sbuffer;
    uint32_t name_len = (strlen(name) + 1) * 2;
    uint32_t off_name = sizeof(TZ_UEFI_SET_REQ);
    uint32_t off_guid = ALIGN(off_name + name_len, 8);
    uint32_t off_data = ALIGN(off_guid + sizeof(EFI_GUID), 8);

    req->CommandId = CMD_UEFI_SET_VARIABLE;
    req->VariableNameOffset = off_name;
    req->VariableNameSize = name_len;
    req->VendorGuidOffset = off_guid;
    req->VendorGuidSize = sizeof(EFI_GUID);
    /* Standard secure attributes: NV | BS | RT | TIME_AUTH */
    req->Attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | 
                      EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS;

    req->DataOffset = off_data;
    req->DataSize = (uint32_t)fsize;

    /* Copy Name (Unicode) */
    uint8_t *ptr_name = (uint8_t *)l_handle->ion_sbuffer + off_name;
    for (int i = 0; i < strlen(name); i++) ptr_name[i*2] = name[i];

    /* Copy GUID */
    const EFI_GUID *g = &EFI_GLOBAL_VARIABLE;
    if (strcmp(name, "db") == 0 || strcmp(name, "dbx") == 0) g = &EFI_DB_VARIABLE;
    memcpy((uint8_t *)l_handle->ion_sbuffer + off_guid, g, sizeof(EFI_GUID));

    /* Copy Data */
    if (fsize > 0) {
        if (off_data + fsize > MAX_BUFFER_SIZE) {
            printf("[ERROR] Buffer overflow (Data too large)\n");
            if (buf) free(buf); pthread_mutex_unlock(&cmd_lock); return -1;
        }
        memcpy((uint8_t *)l_handle->ion_sbuffer + off_data, buf, fsize);
    }

    req->Length = off_data + fsize;
    int ret = QSEECom_send_modified_cmd(l_handle, 
            l_handle->ion_sbuffer, ALIGN(req->Length, 64), 
            l_handle->ion_sbuffer, MAX_BUFFER_SIZE, &ion_fd_info);

    TZ_UEFI_SET_RSP *rsp = (TZ_UEFI_SET_RSP *)l_handle->ion_sbuffer;
    uint32_t status = rsp->Status;

    pthread_mutex_unlock(&cmd_lock);
    if (buf) free(buf);

    if (ret == 0 && status == EFI_SUCCESS) {
        printf("[INFO] Operation Success.\n");
    } else {
        printf("[ERROR] Operation Failed. TZ_Ret: %d, EFI_Status: 0x%X\n", ret, status);
    }
    return (int)status;
}

/* ===================================================================
   MAIN
   =================================================================== */

void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("  -c, --checkStatus           Check Secure Boot status and keys\n");
    printf("  -e, --enroll                Update/Enroll a key (requires -v and -f)\n");
    printf("  -v, --var <name>            Variable Name (PK, KEK, db, dbx)\n");
    printf("  -f, --file <path>           Input .auth/.esl file path\n");
}

int main(int argc, char *argv[]) {
    int ret = QSEECom_start_app(&l_handle, TA_APP_PATH, TA_APP_NAME, MAX_BUFFER_SIZE);
    if (ret != 0) {
        printf("[CRITICAL] Failed to load TrustZone App: %d\n", ret);
        return -1;
    }

    int mode = 0; /* 1: check, 2: enroll, 3: append */
    char *var_name = NULL;
    char *file_path = NULL;

    static struct option long_options[] = {
        {"checkStatus", no_argument, 0, 'c'},
        {"enroll",      no_argument, 0, 'e'},
        {"var",         required_argument, 0, 'v'},
        {"file",        required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "ceav:f:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': mode = 1; break;
            case 'e': mode = 2; break;
            case 'v': var_name = optarg; break;
            case 'f': file_path = optarg; break;
            default: print_usage(argv[0]); return -1;
        }
    }

    if (mode == 0) {
        print_usage(argv[0]);
    } else if (mode == 1) {
        uint32_t status, size;

	printf("\n=== Key Database ===\n");
        const char *keys[] = {"PK", "KEK", "db", "dbx"};
        for (int i=0; i<4; i++) {
            uefi_send_get(keys[i], 0, &status, &size, NULL);
            printf(" [%3s] ", keys[i]);
            if (status == EFI_SUCCESS || status == EFI_BUFFER_TOO_SMALL) {
                printf("Present (Size: %d)\n", size);
            } else if (status == EFI_NOT_FOUND) {
                printf("Not Provisioned\n");
            } else {
                printf("Error 0x%X\n", status);
            }
        }
    } else if (mode == 2) {
        if (!var_name) {
            printf("[ERROR] -v <varname> is required.\n");
        }
        else if (!file_path) {
            printf("[ERROR] -f <file> is required.\n");
        }
        else {
            /* CONSTRAINT 1: Block APPEND on PK */
            if (strcmp(var_name, "PK") == 0) {
                 printf("[ERROR] [SECURITY BLOCK] Modification of 'PK' (Platform Key) is strictly PROHIBITED.\n");
                 QSEECom_shutdown_app(&l_handle);
                 return -1;
            }

            /* CONSTRAINT 2: Valid Variable Check */
            if (strcmp(var_name, "db") != 0 &&
                strcmp(var_name, "dbx") != 0 &&
                strcmp(var_name, "KEK") != 0 &&
                strcmp(var_name, "PK") != 0) {
                printf("[ERROR] Variable not supported. Use PK, db, dbx, or KEK.\n");
                QSEECom_shutdown_app(&l_handle);
                return -1;
            }

            printf("Mode: ENROLL/UPDATE %s --> Operation will overwrite the entries\n", var_name);
            uefi_set_variable(var_name, file_path);
        }
    }

    QSEECom_shutdown_app(&l_handle);
    return 0;
}

