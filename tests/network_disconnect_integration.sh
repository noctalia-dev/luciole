#!/usr/bin/env bash
# Destructive fault fixture. Run only in an otherwise unused systemd VM.
set -euo pipefail
[[ ${LUCIOLE_TEST_DISPOSABLE_VM:-} == 1 && $EUID == 0 ]]
systemd-detect-virt --vm >/dev/null
exec 9>/run/luciole-disconnect-test.lock
flock -n 9
probe=$(realpath "${1:?Pass the network-process-test executable}")
id lucioletest >/dev/null 2>&1 || useradd -m lucioletest
target=/media/lucioletest/disconnect
export_root=/srv/luciole-disconnect
mount_unit=$(systemd-escape --path --suffix=mount "$target")
auto_unit=$(systemd-escape --path --suffix=automount "$target")
cleanup() {
  nft delete table inet luciole_disconnect 2>/dev/null || true
  umount -l "$target" 2>/dev/null || true
  systemctl stop "$auto_unit" "$mount_unit" || true
  rm -f "/run/systemd/system/$mount_unit" "/run/systemd/system/$auto_unit" /etc/exports.d/luciole-disconnect.exports
  systemctl daemon-reload
  exportfs -ra
  rm -rf "$export_root"
}
trap cleanup EXIT
mkdir -p "$export_root" /etc/exports.d "$target"
chmod 0777 "$export_root"
printf 'NAS fixture\n' > "$export_root/marker"
printf '%s' 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLbtAAAAABJRU5ErkJggg==' | base64 -d > "$export_root/preview.png"
truncate -s 4G "$export_root/large"
printf '%s 127.0.0.1(rw,sync,no_subtree_check,no_root_squash,fsid=0)\n' "$export_root" > /etc/exports.d/luciole-disconnect.exports
systemctl start nfs-server
exportfs -ra
cat > "/run/systemd/system/$mount_unit" <<UNIT
[Mount]
What=127.0.0.1:/
Where=$target
Type=nfs
Options=vers=4.2,hard,timeo=600,retrans=2,lookupcache=none,actimeo=0
TimeoutSec=3s
UNIT
cat > "/run/systemd/system/$auto_unit" <<UNIT
[Automount]
Where=$target
TimeoutIdleSec=60s
UNIT
systemctl daemon-reload
systemctl start "$auto_unit"
for mode in --disconnect --shutdown; do
  coproc CLIENT { runuser -u lucioletest -- env TMPDIR=/var/tmp GIO_USE_VFS=local G_DEBUG=fatal-warnings "$probe" "$mode" "$target"; }
  client_pid=$CLIENT_PID
  client_output=${CLIENT[0]}
  client_input=${CLIENT[1]}
  read -r ready <&"$client_output"
  [[ $ready == 'ready for disconnect' ]]
  nft add table inet luciole_disconnect
  nft 'add chain inet luciole_disconnect output { type filter hook output priority 0; policy accept; }'
  nft add rule inet luciole_disconnect output tcp dport 2049 drop
  printf g >&"$client_input"
  read -r verdict <&"$client_output"
  printf '%s\n' "$verdict"
  if [[ $mode == --shutdown ]]; then
    [[ $verdict == 'shutdown before reconnect passed' ]]
    timeout 5s tail --pid="$client_pid" -f /dev/null
    wait "$client_pid"
    nft delete table inet luciole_disconnect
  else
    [[ $verdict == 'disconnect browsing, preview and mutation passed;'* ]]
    nft delete table inet luciole_disconnect
    printf r >&"$client_input"
    cat <&"$client_output"
    wait "$client_pid"
  fi
done
printf 'hard-NFS disconnect and shutdown integration passed\n'
