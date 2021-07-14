/* SPDX-License-Identifier: LGPL-2.1+ */

#include <efi.h>
#include <efilib.h>
#include <libfdt.h>

#include "linux.h"
#include "util.h"

#ifdef __i386__
#define __regparm0__ __attribute__((regparm(0)))
#else
#define __regparm0__
#endif

typedef VOID(*handover_f)(VOID *image, EFI_SYSTEM_TABLE *table, struct boot_params *params) __regparm0__;
static VOID linux_efi_handover(EFI_HANDLE image, struct boot_params *params) {
        handover_f handover;
        UINTN start = (UINTN)params->hdr.code32_start;

#ifdef __x86_64__
        asm volatile ("cli");
        start += 512;
#endif
        handover = (handover_f)(start + params->hdr.handover_offset);
        handover(image, ST, params);
}

EFI_STATUS linux_exec(EFI_HANDLE *image,
                      CHAR8 *cmdline, UINTN cmdline_len,
                      UINTN linux_addr,
                      UINTN initrd_addr, UINTN initrd_size) {
        struct boot_params *image_params;
        struct boot_params *boot_params;
        UINT8 setup_sectors;
        EFI_PHYSICAL_ADDRESS addr;
        EFI_STATUS err;

        image_params = (struct boot_params *) linux_addr;

        if (image_params->hdr.boot_flag != 0xAA55 ||
            image_params->hdr.header != SETUP_MAGIC ||
            image_params->hdr.version < 0x20b ||
            !image_params->hdr.relocatable_kernel)
                return EFI_LOAD_ERROR;

        boot_params = (struct boot_params *) 0xFFFFFFFF;
        err = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, EfiLoaderData,
                                EFI_SIZE_TO_PAGES(0x4000), (EFI_PHYSICAL_ADDRESS*) &boot_params);
        if (EFI_ERROR(err))
                return err;

        ZeroMem(boot_params, 0x4000);
        CopyMem(&boot_params->hdr, &image_params->hdr, sizeof(struct setup_header));
        boot_params->hdr.type_of_loader = 0xff;
        setup_sectors = image_params->hdr.setup_sects > 0 ? image_params->hdr.setup_sects : 4;
        boot_params->hdr.code32_start = (UINT32)linux_addr + (setup_sectors + 1) * 512;

        if (cmdline) {
                addr = 0xA0000;
                err = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, EfiLoaderData,
                                        EFI_SIZE_TO_PAGES(cmdline_len + 1), &addr);
                if (EFI_ERROR(err))
                        return err;
                CopyMem((VOID *)(UINTN)addr, cmdline, cmdline_len);
                ((CHAR8 *)(UINTN)addr)[cmdline_len] = 0;
                boot_params->hdr.cmd_line_ptr = (UINT32)addr;
        }

        boot_params->hdr.ramdisk_image = (UINT32)initrd_addr;
        boot_params->hdr.ramdisk_size = (UINT32)initrd_size;

        linux_efi_handover(image, boot_params);
        return EFI_LOAD_ERROR;
}

static void *open_fdt(void) {
        EFI_STATUS status;
        void *fdt;
        unsigned long fdt_size;

        /* Look for a device tree configuration table entry. */
        status = LibGetSystemConfigurationTable(&EfiDtbTableGuid,
                                                (VOID**) &fdt);
        if (EFI_ERROR(status)) {
                Print(L"DTB table not found\n");
                return 0;
        }

        if (fdt_check_header(fdt) != 0) {
		Print(L"Invalid header detected on UEFI supplied FDT\n");
		return 0;
	}
	fdt_size = fdt_totalsize(fdt);
        Print(L"Size of fdt is %lu\n", fdt_size);

        return fdt;
}

#ifndef fdt_setprop_var
#define fdt_setprop_var(fdt, node_offset, name, var) \
	fdt_setprop((fdt), (node_offset), (name), &(var), sizeof(var))
#endif

// Update fdt /chosen module with initrd address and size
// TODO we are updating in-place, probably we need to copy around and
// then update the configuration table to be safe, although usually
// you have some slack in the dtb, I think.
static void update_fdt(UINTN initrd_addr, UINTN initrd_size) {
        EFI_STATUS status;
        void *fdt;
	int node, num_rsv;
        uint64_t initrd_start, initrd_end;

        fdt = open_fdt();
        if (fdt == 0)
                return;

        node = fdt_subnode_offset(fdt, 0, "chosen");
	if (node < 0) {
		node = fdt_add_subnode(fdt, 0, "chosen");
		if (node < 0) {
			/* 'node' is an error code when negative: */
			status = node;
                        Print(L"Error creating chosen\n");
			return;
		}
	}

        initrd_start = cpu_to_fdt64(initrd_addr);
        initrd_end = cpu_to_fdt64(initrd_addr + initrd_size);

        status = fdt_setprop_var(fdt, node, "linux,initrd-start", initrd_start);
        if (status) {
                Print(L"Cannot create initrd-start property\n");
                return;
        }

        status = fdt_setprop_var(fdt, node, "linux,initrd-end", initrd_end);
        if (status) {
                Print(L"Cannot create initrd-end property\n");
                return;
        }
}

// linux_addr: .linux section address
EFI_STATUS linux_aarch64_exec(EFI_HANDLE image,
                              CHAR8 *cmdline, UINTN cmdline_len,
                              UINTN linux_addr,
                              UINTN initrd_addr, UINTN initrd_size) {
        struct arm64_kernel_header *hdr;
        struct arm64_linux_pe_header *pe;
        handover_f handover;

        if (initrd_size != 0)
                update_fdt(initrd_addr, initrd_size);

        hdr = (struct arm64_kernel_header *) linux_addr;

        pe = (void *)((UINTN)linux_addr + hdr->hdr_offset);
        handover = (handover_f)((UINTN)linux_addr + pe->opt.entry_addr);

        Print(L"Calling now EFI kernel stub\n");

        handover(image, ST, image);

        return EFI_LOAD_ERROR;
}
