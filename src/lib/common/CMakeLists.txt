target_add_lib(client-lib-common common numa rt)

# Add CUDA/GDR support if enabled
if(HF3FS_ENABLE_GDR)
    target_add_gdr_support(client-lib-common SCOPE PRIVATE)
endif()
