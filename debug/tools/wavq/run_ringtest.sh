#!/bin/bash
# RingTone regression on the host JVM, current repo RingTone.java + the Sound stub (FREQ_TABLE only). Output goes to "$W/ringtest.out".
export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; W=$(mktemp -d); cd "$W"
rm -rf out_new && mkdir -p out_new/com/nokia/mid/sound
cp "$HERE/../../../vita-port/nokia_src/RingTone.java" out_new/com/nokia/mid/sound/RingTone.java
cp "$HERE/SoundStub.java" out_new/com/nokia/mid/sound/Sound.java
cp "$HERE/RingToneTest.java" out_new/com/nokia/mid/sound/RingToneTest.java
{
  echo "== compile"
  $JAVA_HOME/bin/javac -nowarn -d out_new out_new/com/nokia/mid/sound/*.java 2>&1 | grep -v '^Note'
  echo "== run"
  $JAVA_HOME/bin/java -cp out_new com.nokia.mid.sound.RingToneTest 2>&1
  echo "== exit $?"
} > "$W/ringtest.out" 2>&1
echo done >> "$W/ringtest.out"
[ -f "$W/ringtest.out" ] && cat "$W/ringtest.out"
