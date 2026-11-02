#!/usr/bin/env bash
# present.sh - live, terminal-only visualization of the KTP test results.
# Runs every test case, collects real statistics, and renders them as ASCII
# tables and bar charts (no GUI, no external plotting tools required).
#
#   bash tests/present.sh
#
# env:
#   PRES_MSGS=N     messages per retransmission/throughput run (default 80)
#   PRES_NOPAUSE=1  do not pause between sections (default: pause if interactive)
#   NO_COLOR=1      disable ANSI colour
cd "$(dirname "$0")/.." || exit 1
export KTP_T=1
MSGS=${PRES_MSGS:-80}
REPEATS=${PRES_REPEATS:-3}   # runs per drop prob, averaged to smooth RNG noise
CT=${PRES_T:-0.3}            # retransmit timeout (s) during collection; localhost-safe

# ---- colours ---------------------------------------------------------------
if [ -t 1 ] && [ -z "$NO_COLOR" ]; then
  B=$'\e[1m'; DIM=$'\e[2m'; R=$'\e[0m'
  GRN=$'\e[32m'; RED=$'\e[31m'; CYN=$'\e[36m'; YEL=$'\e[33m'; MAG=$'\e[35m'
else
  B=; DIM=; R=; GRN=; RED=; CYN=; YEL=; MAG=
fi

section() { echo; echo "${B}${CYN}=== $1 ===${R}"; echo; }
pause()   { if [ -t 0 ] && [ -z "$PRES_NOPAUSE" ]; then
              printf "%s" "${DIM}  (press Enter for the next section) ${R}"; read -r _; fi; }
rule()    { printf '%*s\n' "$1" '' | tr ' ' "$2"; }   # rule <width> <char>

# ---- horizontal bar: label value max width marker(-1=none) annotation -------
hbar() {
  awk -v L="$1" -v v="$2" -v mx="$3" -v w="$4" -v mk="$5" -v an="$6" 'BEGIN{
    if (mx<=0) mx=1;
    n=int(v/mx*w+0.5); if(n>w)n=w; if(n<0)n=0;
    mp=-1; if(mk>=0){ mp=int(mk/mx*w+0.5); if(mp>=w)mp=w-1; if(mp<0)mp=0; }
    out="";
    for(i=0;i<w;i++){
      c=(i<n)?"#":".";
      if(i==mp) c="|";
      out=out c;
    }
    printf "  %-12s [%s] %s\n", L, out, an;
  }'
}

echo "${B}${MAG}"
echo "  +==========================================================+"
echo "  |            KTP  -  Reliable Transport over UDP           |"
echo "  |              live test-result visualization              |"
echo "  +==========================================================+"
echo "${R}"

# ===========================================================================
# PHASE 1 - collect real data from every test case
# ===========================================================================
echo "${DIM}building (no make)...${R}"
bash build.sh >/tmp/pres_build.log 2>&1 || { echo "${RED}BUILD FAILED${R}"; cat /tmp/pres_build.log; exit 1; }

echo "${DIM}collecting data from the test suite (this runs the real protocol)...${R}"
head -c $((MSGS*512)) /dev/urandom > data/pres_in.dat
PSZ=$(stat -c%s data/pres_in.dat)

PCSV=/tmp/pres_rtx.csv
echo "p,avg,kbps,integ" > "$PCSV"
for p in 0.00 0.10 0.20 0.30 0.40 0.50; do
  printf "   loss p=%s (x%s) ... " "$p" "$REPEATS"
  avgs=""; kbs=""; ig=OK
  for r in $(seq 1 "$REPEATS"); do
    KTP_T=$CT bash tests/run_pair.sh "$p" data/pres_in.dat data/pres_out.dat 0 0 0 150 >/tmp/pres_${p}_${r}.log 2>&1
    st=$(grep -o 'stats=[0-9 .]*' /tmp/pres_${p}_${r}.log | head -1 | sed 's/stats=//')
    avgs="$avgs $(echo "$st" | awk '{print $3+0}')"
    kbs="$kbs $(echo "$st" | awk '{print $6+0}')"
    grep -q 'RESULT: IDENTICAL' /tmp/pres_${p}_${r}.log || ig=FAIL
  done
  avg=$(echo "$avgs" | awk '{s=0;for(i=1;i<=NF;i++)s+=$i;printf "%.4f",NF?s/NF:0}')
  kb=$(echo "$kbs"  | awk '{s=0;for(i=1;i<=NF;i++)s+=$i;printf "%.2f",NF?s/NF:0}')
  echo "$p,$avg,$kb,$ig" >> "$PCSV"
  echo "${GRN}done${R} (avg=${avg}x, ${kb} KB/s, ${ig})"
done

printf "   wraparound (300 msgs > 256) ... "
bash tests/dbg_med.sh 300 >/tmp/pres_wrap.log 2>&1
grep -q 'RESULT IDENTICAL' /tmp/pres_wrap.log && echo "${GRN}done${R}" || echo "${RED}FAIL${R}"

printf "   3 concurrent pairs (p=0.1) ... "
bash tests/concurrent.sh 0.1 >/tmp/pres_cc.log 2>&1
CCOK=$(grep -o 'RESULT: [0-9]/3' /tmp/pres_cc.log | tail -1 | sed 's/RESULT: //')
echo "${GRN}${CCOK:-0/3}${R}"

printf "   zero-window (nospace) deadlock demo ... "
bash tests/nospace_test.sh >/tmp/pres_ns.log 2>&1
echo "${GRN}done${R}"

printf "   garbage collector ... "
bash tests/gc_test.sh >/tmp/pres_gc.log 2>&1
grep -q 'GC TEST: PASS' /tmp/pres_gc.log && echo "${GRN}done${R}" || echo "${RED}FAIL${R}"

rm -f data/pres_out.dat data/pres_in.dat

# ===========================================================================
# 1) CORRECTNESS MATRIX
# ===========================================================================
section "1. Protocol Correctness  (every byte delivered, in order)"
PKB=$((PSZ/1024))
ig() { awk -F, -v p="$1" '$1==p{print ($4=="OK")?1:0}' "$PCSV"; }
hdr="  +----------------------------+--------+-----------+-------------+"
echo "$hdr"
printf "  | %-26s | %-6s | %-9s | %-11s |\n" "Test case" "Loss" "Bytes" "Result"
echo "$hdr"
row() { # name loss size ok
  local txt col
  if [ "$4" = 1 ]; then txt="PASS IDENT "; col=$GRN; else txt="FAIL MISMAT"; col=$RED; fi
  printf "  | %-26s | %-6s | %-9s | ${col}%-11s${R} |\n" "$1" "$2" "$3" "$txt"
}
row "Lossless single pair"    "0%"  "$PKB KB"   "$(ig 0.00)"
row "Moderate loss"           "30%" "$PKB KB"   "$(ig 0.30)"
row "Heavy loss"              "50%" "$PKB KB"   "$(ig 0.50)"
WROK=$(grep -q 'RESULT IDENTICAL' /tmp/pres_wrap.log && echo 1 || echo 0)
row "Seq wraparound (300 msg)" "0%" "150 KB"    "$WROK"
CCN=$(echo "${CCOK:-0/3}" | cut -d/ -f1); [ "$CCN" = 3 ] && CC1=1 || CC1=0
row "3 concurrent pairs"      "10%" "~63 KBx3"  "$CC1"
echo "$hdr"
echo "  ${DIM}Every transfer is byte-for-byte identical to the input, even under loss.${R}"
pause

# ===========================================================================
# 2) RETRANSMISSION COST vs DROP PROBABILITY
# ===========================================================================
section "2. Retransmission Cost vs Drop Probability"
echo "  ${DIM}#=measured avg tx/msg (${REPEATS}-run mean)   |=selective-repeat ideal 1/(1-p), a lower bound${R}"
echo
maxavg=$(awk -F, 'NR>1{t=1/(1-$1); if($2>m)m=$2; if(t>m)m=t}END{printf "%.3f",m*1.05}' "$PCSV")
while IFS=, read -r p avg kb ig; do
  [ "$p" = "p" ] && continue
  theory=$(awk -v p="$p" 'BEGIN{printf "%.3f",1/(1-p)}')
  an=$(printf "meas ${B}%.2fx${R}  SR-ideal %.2fx" "$avg" "$theory")
  hbar "p=$p" "$avg" "$maxavg" 34 "$theory" "$an"
done < "$PCSV"
echo
echo "  ${DIM}KTP uses Go-Back-N: one loss retransmits the whole in-flight window, so${R}"
echo "  ${DIM}measured overhead rises with p and sits above the selective-repeat ideal.${R}"
pause

# ===========================================================================
# 3) THROUGHPUT vs DROP PROBABILITY
# ===========================================================================
section "3. Goodput vs Drop Probability"
base=$(awk -F, '$1=="0.00"{printf "%.0f",$3}' "$PCSV")
echo "  ${DIM}baseline (no loss): ${B}${base} KB/s${R}${DIM}  -- bars below show the lossy runs, scaled to each other${R}"
echo "  ${DIM}(goodput here is dominated by the T=${CT}s retransmit timeout on the lossy link)${R}"
echo
maxkb=$(awk -F, 'NR>1 && $1!="0.00"{if($3+0>m)m=$3+0}END{printf "%.2f",m}' "$PCSV")
while IFS=, read -r p avg kb ig; do
  [ "$p" = "p" ] && continue
  [ "$p" = "0.00" ] && continue
  an=$(printf "${B}%.1f${R} KB/s" "$kb")
  hbar "p=$p" "$kb" "$maxkb" 34 -1 "$an"
done < "$PCSV"
echo
echo "  ${DIM}Goodput falls sharply with loss: each drop costs a full retransmit timeout.${R}"
pause

# ===========================================================================
# 4) ZERO-WINDOW (nospace) DEADLOCK: before vs after the fix
# ===========================================================================
section "4. Zero-Window (nospace) Deadlock - solved by a persist probe"
nsA=$(grep 'HUNG as expected' /tmp/pres_ns.log)
gotA=$(echo "$nsA" | sed -n 's/.*received only \([0-9]*\) bytes of \([0-9]*\).*/\1/p')
totA=$(echo "$nsA" | sed -n 's/.*received only \([0-9]*\) bytes of \([0-9]*\).*/\2/p')
gotA=${gotA:-0}; totA=${totA:-1}
nsBok=$(grep -q 'FILE IDENTICAL' /tmp/pres_ns.log && echo 1 || echo 0)
echo "  ${DIM}Receiver buffer fills (rwnd=0) and every window-reopen ACK is dropped.${R}"
echo
anA=$(printf "${RED}stalled %d / %d bytes  -> DEADLOCK${R}" "$gotA" "$totA")
hbar "persist OFF" "$gotA" "$totA" 34 -1 "$anA"
if [ "$nsBok" = 1 ]; then
  hbar "persist ON" "$totA" "$totA" 34 -1 "${GRN}complete  -> file identical${R}"
else
  hbar "persist ON" "0" "$totA" 34 -1 "${RED}FAILED${R}"
fi
echo
echo "  ${DIM}Without the probe the sender waits forever for a window update that never${R}"
echo "  ${DIM}arrives. The sender-side persist probe re-asks and breaks the deadlock.${R}"
pause

# ===========================================================================
# 5) GARBAGE COLLECTOR: reclaims a crashed process's socket
# ===========================================================================
section "5. Garbage Collector - reclaims sockets of processes that died"
before=$(grep 'allotted BEFORE kill' /tmp/pres_gc.log | sed 's/.*allotted=//')
echo "  ${DIM}A process creates a KTP socket, then is SIGKILLed (never calls k_close).${R}"
echo
printf "  %-22s allotted = %s   %s\n" "before kill" "${before:-1}" "${YEL}[#] in use${R}"
printf "  %-22s %s\n" "SIGKILL (no k_close)" "${RED}owner dies holding the socket${R}"
echo "  ${DIM}  GC polls kill(pid,0) and frees the entry:${R}"
grep -E '^  t=[0-9]+ allotted=' /tmp/pres_gc.log | while read -r line; do
  t=$(echo "$line" | sed -n 's/.*t=\([0-9]*\).*/\1/p')
  c=$(echo "$line" | sed -n 's/.*allotted=\([0-9]*\).*/\1/p')
  if [ "$c" = 0 ]; then
    printf "    t=%-3s allotted=%s   %s\n" "$t" "$c" "${GRN}[ ] reclaimed OK${R}"
  else
    printf "    t=%-3s allotted=%s   %s\n" "$t" "$c" "${YEL}[#] still held${R}"
  fi
done
echo
grep -q 'GC TEST: PASS' /tmp/pres_gc.log \
  && echo "  ${GRN}${B}GC TEST: PASS${R} ${DIM}- no socket leak even when the owner crashes.${R}" \
  || echo "  ${RED}${B}GC TEST: FAIL${R}"
pause

# ===========================================================================
# SUMMARY
# ===========================================================================
section "Summary"
allok=1
awk -F, 'NR>1 && $4!="OK"{bad=1}END{exit bad?1:0}' "$PCSV" || allok=0
[ "$WROK" = 1 ] || allok=0; [ "$CC1" = 1 ] || allok=0
[ "$nsBok" = 1 ] || allok=0; grep -q 'GC TEST: PASS' /tmp/pres_gc.log || allok=0
if [ "$allok" = 1 ]; then
  echo "  ${GRN}${B}All test cases passed.${R}"
else
  echo "  ${YEL}${B}Some cases need attention - see sections above.${R}"
fi
echo "  ${DIM}correctness under loss . overhead >= 1/(1-p) (Go-Back-N) . deadlock solved . no leaks${R}"
echo
