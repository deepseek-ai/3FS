target_add_lib(simple_example core-app core-user core-service fdb simple_example-fbs mgmtd-client storage-client memory-common analytics)
target_add_bin(simple_example_main "main.cpp" simple_example)
