// Minimal stub bindings for userfaultfd-sys
// This is just to get things compiling - real implementation would need proper bindings

// IOCTL magic numbers
pub const USERFAULTFD_IOC: u32 = 0xAA;
pub const UFFDIO: u32 = 0xAA;

pub const UFFD_API: u64 = 0xAA;
pub const UFFD_API_FEATURES: u64 = 0;
pub const UFFD_API_IOCTLS: u64 = 0;

pub const UFFD_FEATURE_EVENT_FORK: u64 = 1 << 1;
pub const UFFD_FEATURE_EVENT_REMAP: u64 = 1 << 2;
pub const UFFD_FEATURE_EVENT_REMOVE: u64 = 1 << 3;
pub const UFFD_FEATURE_MISSING_HUGETLBFS: u64 = 1 << 4;
pub const UFFD_FEATURE_MISSING_SHMEM: u64 = 1 << 5;
pub const UFFD_FEATURE_EVENT_UNMAP: u64 = 1 << 6;
pub const UFFD_FEATURE_SIGBUS: u64 = 1 << 7;
pub const UFFD_FEATURE_THREAD_ID: u64 = 1 << 8;
pub const UFFD_FEATURE_PAGEFAULT_FLAG_WP: u64 = 1 << 9;
pub const UFFD_FEATURE_MINOR_HUGETLBFS: u64 = 1 << 10;
pub const UFFD_FEATURE_MINOR_SHMEM: u64 = 1 << 11;
pub const UFFD_FEATURE_EXACT_ADDRESS: u64 = 1 << 12;
pub const UFFD_FEATURE_WP_HUGETLBFS_SHMEM: u64 = 1 << 13;

pub const UFFDIO_API: u64 = 0xc018aa3f;
pub const UFFDIO_REGISTER: u64 = 0xc020aa00;
pub const UFFDIO_UNREGISTER: u64 = 0x8010aa01;
pub const UFFDIO_WAKE: u64 = 0x8010aa02;
pub const UFFDIO_COPY: u64 = 0xc028aa03;
pub const UFFDIO_ZEROPAGE: u64 = 0xc020aa04;
pub const UFFDIO_WRITEPROTECT: u64 = 0xc018aa06;
pub const UFFDIO_CONTINUE: u64 = 0xc020aa07;

// These are bit positions for ioctl numbers (used for bitflags)
pub const _UFFDIO_API: u64 = 0x3F;
pub const _UFFDIO_REGISTER: u64 = 0x00;
pub const _UFFDIO_UNREGISTER: u64 = 0x01;
pub const _UFFDIO_WAKE: u64 = 0x02;
pub const _UFFDIO_COPY: u64 = 0x03;
pub const _UFFDIO_ZEROPAGE: u64 = 0x04;
pub const _UFFDIO_WRITEPROTECT: u64 = 0x06;
pub const _UFFDIO_CONTINUE: u64 = 0x07;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffdio_api {
    pub api: u64,
    pub features: u64,
    pub ioctls: u64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffdio_range {
    pub start: u64,
    pub len: u64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffdio_register {
    pub range: uffdio_range,
    pub mode: u64,
    pub ioctls: u64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffdio_copy {
    pub dst: u64,
    pub src: u64,
    pub len: u64,
    pub mode: u64,
    pub copy: i64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffdio_zeropage {
    pub range: uffdio_range,
    pub mode: u64,
    pub zeropage: i64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffd_msg {
    pub event: u8,
    pub reserved1: u8,
    pub reserved2: u16,
    pub reserved3: u32,
    pub arg: uffd_msg_arg,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union uffd_msg_arg {
    pub pagefault: uffd_msg_pagefault,
    pub fork: uffd_msg_fork,
    pub remap: uffd_msg_remap,
    pub remove: uffd_msg_remove,
    pub reserved: uffd_msg_reserved,
}

impl std::fmt::Debug for uffd_msg_arg {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("uffd_msg_arg").finish()
    }
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffd_msg_pagefault {
    pub flags: u64,
    pub address: u64,
    pub feat: uffd_msg_pagefault_feat,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union uffd_msg_pagefault_feat {
    pub ptid: u32,
}

impl std::fmt::Debug for uffd_msg_pagefault_feat {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("uffd_msg_pagefault_feat").finish()
    }
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffd_msg_fork {
    pub ufd: u32,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffd_msg_remap {
    pub from: u64,
    pub to: u64,
    pub len: u64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffd_msg_remove {
    pub start: u64,
    pub end: u64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct uffd_msg_reserved {
    pub reserved1: u64,
    pub reserved2: u64,
    pub reserved3: u64,
}

pub const UFFD_EVENT_PAGEFAULT: u8 = 0x12;
pub const UFFD_EVENT_FORK: u8 = 0x13;
pub const UFFD_EVENT_REMAP: u8 = 0x14;
pub const UFFD_EVENT_REMOVE: u8 = 0x15;
pub const UFFD_EVENT_UNMAP: u8 = 0x16;

pub const UFFD_PAGEFAULT_FLAG_WRITE: u64 = 1 << 0;
pub const UFFD_PAGEFAULT_FLAG_WP: u64 = 1 << 1;
pub const UFFD_PAGEFAULT_FLAG_MINOR: u64 = 1 << 2;

pub const UFFDIO_REGISTER_MODE_MISSING: u64 = 1 << 0;
pub const UFFDIO_REGISTER_MODE_WP: u64 = 1 << 1;
pub const UFFDIO_REGISTER_MODE_MINOR: u64 = 1 << 2;

pub const UFFDIO_COPY_MODE_DONTWAKE: u64 = 1 << 0;
pub const UFFDIO_COPY_MODE_WP: u64 = 1 << 1;
pub const UFFDIO_ZEROPAGE_MODE_DONTWAKE: u64 = 1 << 0;
// User mode only flag
pub const UFFD_USER_MODE_ONLY: i32 = 1;
