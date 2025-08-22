#ifndef EVENT
#define EVENT(...)
#endif

#ifndef PAIR_EVENT
#define PAIR_EVENT(...)
#endif

PAIR_EVENT(Fdb, NewTransaction, 0);
PAIR_EVENT(Fdb, CommitTransaction, 1);
PAIR_EVENT(Fdb, CancelTransaction, 2);

PAIR_EVENT(Meta, LoadInode, 0);
PAIR_EVENT(Meta, SnapshotLoadInode, 1);
PAIR_EVENT(Meta, LoadDirEntry, 2);
PAIR_EVENT(Meta, SnapshotLoadDirEntry, 3);
