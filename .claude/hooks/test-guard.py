#!/usr/bin/env python3
"""Cases for guard-bash.py. Run: python3 .claude/hooks/test-guard.py"""
import json, subprocess, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
G = "$S/g2ssh.exp 192.168.40.246"
cases = [
    # (expected, command)
    ("deny", G + " 'cat /mnt/extsd/boot-logs/count; sync; (sleep 1; reboot) > /dev/null 2>&1 &'"),
    ("deny", G + " 'reboot'"),
    ("deny", G + " 'sync && /sbin/poweroff'"),
    ("deny", "ssh root@192.168.40.246 'echo b > /proc/sysrq-trigger'"),
    ("deny", G + " 'LD_TRACE_LOADED_OBJECTS=1 /mnt/app/app/HDZGOGGLE'"),
    ("deny", G + " 'cd /tmp; /tmp/HDZGOGGLE &'"),
    ("deny", G + " 'GLOG_minloglevel=3 /mnt/app/app/HDZGOGGLE'"),
    ("ask", "cp ~/Downloads/hdzero-goggle2-recovery/HDZG_OS.bin /Volumes/HDZERO_BOOT/"),
    ("ask", "cp build_goggle2/out/HDZERO_GOGGLE2-9.6.0.bin \"/Volumes/NO NAME2/\""),
    ("ask", G + " 'cp /mnt/extsd/goggle2-flash/rc.sh /mnt/app/script/rc.sh; sync'"),
    ("ask", G + " 'sh /mnt/extsd/goggle2-install-hooks.sh g2'"),
    ("ask", G + " 'fw_setenv bootdelay 1'"),
    ("ask", G + " 'echo x > /mnt/app/setting.ini'"),
    ("ask", G + " 'rm /overlay/upperdir/etc/init.d/rc.final'"),
    ("ask", "scp -O rc.sh root@192.168.40.246:/mnt/app/script/rc.sh"),
    ("ask", "sudo ifconfig en0 alias 192.168.100.50 255.255.255.0"),
    ("ask", "networksetup -setmanual Wi-Fi 192.168.1.5 255.255.255.0 192.168.1.1"),
    ("ask", G + " 'D=/etc/init.d/rc.final; N=/mnt/extsd/x; cp $N $D.new && mv $D.new $D; sync'"),
    ("ask", G + " 'cd /mnt/app/script && cp /mnt/extsd/rc.sh rc.sh'"),
    ("ask", G + " 'F=\"/overlay/upperdir/etc/init.d/rc.final\"; rm $F'"),
    ("deny", G + " 'LD_LIBRARY_PATH=/tmp/lib:$LD_LIBRARY_PATH /tmp/HDZGOGGLE &'"),
    # must pass
    ("pass", G + " 'grep -rn reboot /mnt/app/script; ls -l /sbin/reboot'"),
    ("pass", G + " 'md5sum /mnt/extsd/HDZGOGGLE /tmp/HDZGOGGLE; pidof HDZGOGGLE; tail -3 /mnt/extsd/HDZGOGGLE.log'"),
    ("pass", G + " 'cp /mnt/app/setting.ini /mnt/extsd/x.ini; cat /etc/init.d/rc.final'"),
    ("pass", "scp -O root@192.168.40.246:/mnt/app/setting.ini ./setting.ini"),
    ("pass", "cp utilities/sdcard-test/rc-exp.sh /Volumes/HDZERO_BOOT/rc-exp.sh"),
    ("pass", "ls -la /Volumes/G2RECOVERY/HDZG_OS.bin; md5 -q /Volumes/G2RECOVERY/HDZG_OS.bin"),
    ("pass", "grep -n 'reboot' mkapp/app/script/recstat.sh"),
    ("pass", "diskutil unmountDisk disk6"),
    ("pass", "ping -c 2 192.168.40.246; nc -z -G 3 192.168.40.246 22"),
    ("pass", "cat utilities/sdcard-test/rc-exp.sh | grep HDZGOGGLE"),
    ("pass", G + " 'D=/etc/init.d/rc.final; md5sum $D; sh -n $D; cat $D'"),
    ("pass", G + " 'P=$(pidof HDZGOGGLE); grep -o x /proc/$P/maps; for p in $(pidof record); do echo $p; done'"),
    ("pass", G + " 'cp /mnt/extsd/rc.final.bootmark /tmp/x; dmesg > /mnt/extsd/d.txt'"),
]
fail = 0
for exp, cmd in cases:
    r = subprocess.run([sys.executable, os.path.join(HERE, "guard-bash.py")],
                       input=json.dumps({"tool_name": "Bash", "tool_input": {"command": cmd}}),
                       capture_output=True, text=True)
    got = json.loads(r.stdout)["hookSpecificOutput"]["permissionDecision"] if r.stdout.strip() else "pass"
    mark = "ok " if got == exp else "NG "
    fail += got != exp
    print(f"{mark} {exp:4} -> {got:4}  {cmd[:90]}")
print("failures:", fail)
sys.exit(1 if fail else 0)
