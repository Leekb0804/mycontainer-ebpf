#!/bin/bash
# usage: sudo /tmp/userns-exp/run_sudo.sh [--toggle-aa]
#  root cases always; with --toggle-aa also re-runs the unprivileged cases with
#  kernel.apparmor_restrict_unprivileged_userns=0 temporarily (restored on exit).
cd /tmp/userns-exp || exit 1
OUT=/tmp/userns-exp/results_sudo.txt
U=${SUDO_UID:-1000}
{
echo "### root map 0 (uid_map '0 0 1')";             ./exp root-map0 map 0
echo "### root map SUDO_UID (uid_map '0 $U 1')";     ./exp root-mapU map "$U"
echo "### root nomap";                               ./exp root-nomap nomap
if [ "$1" = "--toggle-aa" ]; then
  OLD=$(sysctl -n kernel.apparmor_restrict_unprivileged_userns)
  trap 'sysctl -qw kernel.apparmor_restrict_unprivileged_userns='"$OLD" EXIT
  sysctl -qw kernel.apparmor_restrict_unprivileged_userns=0
  echo "### user map (aa restrict=0)";   sudo -u "#$U" ./exp userAA0-map map "$U"
  echo "### user nomap (aa restrict=0)"; sudo -u "#$U" ./exp userAA0-nomap nomap
fi
} 2>&1 | tee "$OUT"
chown "$U" "$OUT"
