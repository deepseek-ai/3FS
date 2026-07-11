target_add_lib(storage-client storage-fbs common mgmtd-client)

if(HF3FS_ENABLE_GDR)
    target_add_gdr_support(storage-client SCOPE PRIVATE)
endif()
