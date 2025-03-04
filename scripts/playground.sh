#!/usr/bin/env bash

############################  GLOBAL VARIABLES
g_container_name="build-3fs-playground"
g_container_image="ubuntu:22.04"

############################  BASIC FUNCTIONS
msg() {
    printf '%b' "$1" >&2
}

success() {
    msg "\33[32m[✔]\33[0m ${1}${2}"
}

############################  FUNCTIONS
create_container() {
    id=$(docker ps --all --format "{{.ID}}" --filter name=${g_container_name})
    if [ -n "${id}" ]; then
        return
    fi

    docker run -v "$(pwd)":"$(pwd)" \
        -dt \
        --cap-add=SYS_PTRACE \
        --security-opt seccomp=unconfined \
        --restart always \
        --env "UID=$(id -u)" \
        --env "USER=${USER}" \
        --env "TZ=Asia/Shanghai" \
        --env "BUILD_DIR=$(pwd)" \
        --hostname "${g_container_name}" \
        --name "${g_container_name}" \
        --workdir "$(pwd)" \
        "${g_container_image}"

    success "create ${g_container_name} (${g_container_image}) success :)\n"
}

enter_container() {
    docker exec \
        -u "$(id -u):$(id -g)" \
        -it \
        --env "TERM=xterm-256color" \
        "${g_container_name}" /bin/bash
}

main() {
    create_container
    enter_container
}

############################  MAIN()
main "$@"
