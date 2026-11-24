#!/bin/bash
# usage: sudo /tmp/userns-exp/run_sudo2.sh
# (a) exp: sudo + uid_map "0 U 1"/gid_map "0 G 1" + setgid(0)/setuid(0)
# (b) flow: sudo cases, then unprivileged cases with apparmor_restrict_unprivileged_userns
#     temporarily 0 (restored on EXIT and re-read/verified at the end).
cd /tmp/userns-exp || exit 1
U=${SUDO_UID:-1000}; G=${SUDO_GID:-1000}
OUT=/tmp/userns-exp/results_sudo2.txt
rm -rf /tmp/userns-exp/flow_* /tmp/userns-exp/run_rootA-*
{
echo "##### (a) exp: sudo, map '0 $U 1'/'0 $G 1', setid"
./exp rootA-setid map "$U" "$G" setid
echo
echo "##### (b) flow: sudo"
for s in 0 1; do for r in 0 1; do ./flow "root-s${s}-r${r}" "$U" "$G" "$s" "$r"; done; done

OLD=$(sysctl -n kernel.apparmor_restrict_unprivileged_userns)
echo "##### restrict before = $OLD"
trap 'sysctl -qw kernel.apparmor_restrict_unprivileged_userns='"$OLD"'; echo "##### restrict restored -> $(sysctl -n kernel.apparmor_restrict_unprivileged_userns)"' EXIT
sysctl -qw kernel.apparmor_restrict_unprivileged_userns=0
echo "##### (b) flow: unprivileged user, restrict=$(sysctl -n kernel.apparmor_restrict_unprivileged_userns)"
for s in 0 1; do for r in 0 1; do sudo -u "#$U" ./flow "userAA0-s${s}-r${r}" "$U" "$G" "$s" "$r"; done; done
sysctl -qw kernel.apparmor_restrict_unprivileged_userns="$OLD"
echo "##### restrict after explicit restore = $(sysctl -n kernel.apparmor_restrict_unprivileged_userns)"
} 2>&1 | tee "$OUT"
chown "$U" "$OUT"
