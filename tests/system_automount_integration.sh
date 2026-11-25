#!/usr/bin/env bash
# Run only in a disposable systemd VM with nfs-utils, nftables, gvfs, dbus-daemon and system-mounts-test.
set -euo pipefail
[[ ${LUCIOLE_TEST_DISPOSABLE_VM:-} == 1 && $EUID == 0 ]]
systemd-detect-virt --vm >/dev/null
probe=$(realpath "${1:?Pass the system-mounts-test executable}")
fixture=$(mktemp -d)
runtime=$(mktemp -d)
firewall=luciole_automount_$$
cp /etc/fstab "$fixture/fstab"
id lucioletest >/dev/null 2>&1 || useradd -m lucioletest
chown lucioletest "$runtime"
native=/media/lucioletest/native
generated='/media/lucioletest/Space #share'
mount_unit=$(systemd-escape --path --suffix=mount "$native")
auto_unit=$(systemd-escape --path --suffix=automount "$native")
generated_mount=$(systemd-escape --path --suffix=mount "$generated")
generated_auto=$(systemd-escape --path --suffix=automount "$generated")
cleanup() {
  # The timeout cancels the client request; stop this fixture's remaining privileged NFS helper in the VM.
  pkill -KILL -f -- 'mount[.]nfs 127[.]0[.]0[.]2:/unreachable /media/lucioletest/manual' || true
  nft delete table inet "$firewall" 2>/dev/null || true
  systemctl stop "$auto_unit" "$generated_auto" "$mount_unit" "$generated_mount" media-lucioletest-manual.mount || true
  cp "$fixture/fstab" /etc/fstab
  rm -f "/run/systemd/system/$mount_unit" "/run/systemd/system/$auto_unit" /etc/exports.d/luciole-test.exports
  systemctl daemon-reload
  exportfs -ra
  rm -rf "$fixture" "$runtime"
}
trap cleanup EXIT
mkdir -p /srv/luciole-automount-test /etc/exports.d "$native" "$generated" /media/lucioletest/manual
printf 'NAS fixture\n' > /srv/luciole-automount-test/marker
printf '/srv/luciole-automount-test 127.0.0.1(rw,sync,no_subtree_check,no_root_squash,fsid=0)\n' > /etc/exports.d/luciole-test.exports
systemctl start nfs-server
exportfs -ra
cat > "/run/systemd/system/$mount_unit" <<EOF
[Mount]
What=127.0.0.1:/
Where=$native
Type=nfs
Options=vers=4.2,soft,timeo=10,retrans=1
TimeoutSec=3s
EOF
cat > "/run/systemd/system/$auto_unit" <<EOF
[Automount]
Where=$native
TimeoutIdleSec=3s
EOF
printf '%s\n' '127.0.0.1:/ /media/lucioletest/Space\040#share nfs vers=4.2,soft,timeo=10,retrans=1,x-systemd.automount,x-systemd.idle-timeout=3s,x-systemd.mount-timeout=3s 0 0' >> /etc/fstab
printf '%s\n' '127.0.0.1:/ /media/lucioletest/manual nfs noauto,user,vers=4.2,soft,timeo=10,retrans=1 0 0' >> /etc/fstab
systemctl daemon-reload
systemctl start "$auto_unit" "$generated_auto"
browse() {
  runuser -u lucioletest -- env XDG_RUNTIME_DIR="$runtime" GVFS_DISABLE_FUSE=1 G_DEBUG=fatal-warnings GIO_USE_VFS=gvfs dbus-run-session -- "$probe" "$@"
}
# Establish the original failure without mounting anything or allowing user mounts in fstab.
if runuser -u lucioletest -- mount --fake "$generated" > "$fixture/original-error" 2>&1; then
  echo 'Expected the unprivileged explicit mount to fail' >&2
  exit 1
fi
grep -F 'Operation not permitted' "$fixture/original-error"
browse --manual /media/lucioletest/manual
for target in "$native" "$generated"; do
  browse --inspect-idle "$target"
  browse --browse "$target"
  browse --browse "$target"
done
# Concurrent tabs share systemd's activation; both must see the exported marker.
browse --browse "$native" > "$fixture/first" &
first=$!
browse --browse "$native" > "$fixture/second" &
second=$!
wait "$first"
wait "$second"
cat "$fixture/first" "$fixture/second"
coproc NATIVE_CHECK { browse --wait-inactive "$native"; }
native_check_pid=$NATIVE_CHECK_PID
read -r ready <&"${NATIVE_CHECK[0]}"
[[ $ready == 'ready for native unit stop' ]]
systemctl stop "$auto_unit" "$mount_unit"
printf 'go\n' >&"${NATIVE_CHECK[1]}"
cat <&"${NATIVE_CHECK[0]}"
wait "$native_check_pid"
systemctl stop "$generated_auto" "$generated_mount"
printf 'underlying local directory\n' > "$generated/underlying-file"
browse --browse-error "$generated"
browse --terminal-stopped "$generated"
# Drop NFS packets on loopback so the mount times out even in a VM with no external route.
nft add table inet "$firewall"
nft "add chain inet $firewall output { type filter hook output priority 0; policy accept; }"
nft add rule inet "$firewall" output ip daddr 127.0.0.2 tcp dport 2049 drop
sed -i 's|What=127.0.0.1:/|What=127.0.0.2:/unreachable|' "/run/systemd/system/$mount_unit"
systemctl daemon-reload
systemctl start "$auto_unit"
browse --browse-error "$native"
sed -i 's|127.0.0.1:/ /media/lucioletest/manual|127.0.0.2:/unreachable /media/lucioletest/manual|' /etc/fstab
systemctl daemon-reload
browse --manual-error /media/lucioletest/manual
echo 'systemd NFS automount integration passed'
