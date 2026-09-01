"""Full inventory of every entry in a `docker export` stream.

Used by ladder.sh --deep. It is the one check in the ladder that uses no
normalization at all: path, size, mode and md5 of every regular file, plus
symlink and hardlink targets. Two images that differ anywhere outside the
server binary show up here, and nowhere else.

Usage:  docker export <cid> | python3 rootfs_manifest.py OUT
"""
import sys
import tarfile
import hashlib

tf = tarfile.open(fileobj=sys.stdin.buffer, mode="r|*")
out = open(sys.argv[1], "w")
n = 0
for m in tf:
    if m.isreg():
        h = hashlib.md5()
        f = tf.extractfile(m)
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
        out.write("F %s %d %o %s\n" % (m.name, m.size, m.mode, h.hexdigest()))
    elif m.issym():
        out.write("L %s -> %s\n" % (m.name, m.linkname))
    elif m.islnk():
        out.write("H %s -> %s\n" % (m.name, m.linkname))
    elif m.isdir():
        out.write("D %s %o\n" % (m.name, m.mode))
    else:
        out.write("O %s type=%s\n" % (m.name, m.type))
    n += 1
out.close()
print("entries:", n)
