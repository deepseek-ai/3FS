# How to create a new service

1. Copy `src/simple_example` to a new subfolder in `src/`.
2. Copy `src/fbs/simple_example` to a new subfolder in `src/fbs/`.
3. Rename `simple_example` to `your_service_name`.
4. Rename `SimpleExample` to `YourSimpleExample`.
5. Add the new cmake projects to `CMakeLists.txt` files in `src/` and `src/fbs/`.

```bash
svr_name='migration'
SrvName='Migration'
mkdir -p "src/$svr_name" && pushd src/simple_example && cp -rf --parents . "../$svr_name" && popd
mkdir -p "src/fbs/$svr_name" && pushd src/fbs/simple_example && cp -rf --parents . "../$svr_name" && popd
find "src/$svr_name" "src/fbs/$svr_name" -type f | xargs sed -i "s/simple_example/$svr_name/g"
find "src/$svr_name" "src/fbs/$svr_name" -type f | xargs sed -i "s/SimpleExample/$SrvName/g"
```
