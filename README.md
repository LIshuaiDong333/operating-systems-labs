# Operating Systems Labs

Operating systems experiment sources, patches, and filesystem exercises.

- `com_os_exp1/xv6-oslab24-hitsz`: xv6 source snapshot and original build scripts.
- `com_os_exp1/commit.patch`: experiment changes.
- `com_os_exp3/commit.patch`: synchronization experiment changes.
- `com_os_exp4`: page-table experiment patch and text code snapshots.
- `com_os_exp5/user-land-filesystem`: FUSE filesystem exercises, driver sources, and test scripts.

The experiment-two directory contained reports only and has no publishable source files. Experiment patches need the matching original xv6 base to apply. Build xv6 in a compatible Linux environment with the RISC-V toolchain and QEMU. The filesystem exercises need FUSE and their documented driver setup. Disk images, compiled drivers, and test disk fixtures are excluded and must be recreated locally.

Original upstream licenses and attribution are retained. Existing Git histories are excluded; this repository contains a new source snapshot.

## Publication scope

This is a reviewed export of local coursework. Word/PDF reports, archives, credentials, personal submission names, build output, and old Git history are excluded. Original source bytes and encodings are preserved except for the anonymous CSAPP identity placeholders. Upstream attribution and license files are retained where supplied.

The root `.gitignore` uses an explicit file allowlist. Review new files for personal information before adding them. Some experiments depend on their original course framework or external development tools.
