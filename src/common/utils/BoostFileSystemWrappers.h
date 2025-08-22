#pragma once

#include "Path.h"
#include "Result.h"

namespace hf3fs {
Result<bool> boostFilesystemExists(const Path &path);

Result<bool> boostFilesystemIsDir(const Path &path);

Result<boost::filesystem::directory_iterator> boostFilesystemDirIter(const Path &path);

Result<bool> boostFilesystemCreateDir(const Path &path);

Result<bool> boostFilesystemCreateDirs(const Path &path);

Result<bool> boostFilesystemRemove(const Path &path);

Result<boost::uintmax_t> boostFilesystemRemoveAll(const Path &path);

Result<bool> boostFilesystemIsEmpty(const Path &path);

Result<Void> boostFilesystemCopyFile(const Path &src, const Path &dst);

Result<Void> boostFilesystemCreateSymlink(const Path &src, const Path &dst);

Result<Void> boostFilesystemCreateHardLink(const Path &src, const Path &dst);

Result<Void> boostFilesystemRename(const Path &oldPath, const Path &newPath);

}  // namespace hf3fs
