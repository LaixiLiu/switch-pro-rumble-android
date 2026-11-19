#!/system/bin/sh
i=0
while [ "$i" -lt 6 ]; do
  line=$(timeout 30 getevent -q 2>/dev/null | grep -m1 -E '0001 007[23] 00000001$')
  code=$(echo "$line" | awk '{print $3}')
  echo "raw=[$line] parsed=[$code]"
  i=$((i+1))
done
echo probe_done
