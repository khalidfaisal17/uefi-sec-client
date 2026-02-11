SUMMARY = "Client application for UEFI Secure Boot TA"
LICENSE = "CLOSED"
# LIC_FILES_CHKSUM is not required for CLOSED license

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI = "file://uefi_sec_client.c \
           file://Makefile"

S = "${WORKDIR}"

DEPENDS += "securemsm qcom-libdmabufheap"

TARGET_CC_ARCH += "${LDFLAGS}"

do_compile() {
    oe_runmake
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 uefi_sec_client ${D}${bindir}
}

FILES:${PN} += "${bindir}/uefi_sec_client"
