from setuptools import setup, find_packages

setup(
    name="hf3fs_utils",
    version="1.0.7",
    description="3FS 命令行工具",
    packages=['hf3fs_utils'],
    install_requires=[
        "click"
    ],
    python_requires=">=3.6",
    scripts=["hf3fs_utils/hf3fs_cli"],
)
