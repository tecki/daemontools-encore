echo '--- supervise properly runs an orphanage'
catexe test.sv/doublesleeper <<EOF
#!/bin/sh
trap "echo doublesleeper caught HUP" 1
../../sleeper
exec ../../sleeper
EOF
catexe test.sv/run <<EOF
#!/bin/sh
./doublesleeper >output &
mv run2 run
echo the first run
exec ../../sleeper
EOF
catexe test.sv/run2 <<EOF
#!/bin/sh
nohup ../../sleeper >output &
../../pgrphack ../../supervise subtest >output2 &
mv run3 run
echo the second run
EOF
catexe test.sv/run3 <<EOF
#!/bin/sh
echo the third run
svc -x .
EOF
touch test.sv/orphanage
mkdir test.sv/subtest
catexe test.sv/subtest/run <<EOF
#!/bin/sh
echo the subtest
exec ../../../sleeper
EOF
supervise test.sv &
until svok test.sv || ! jobs %% >/dev/null 2>&1
do
  sleep 1
done
if svok test.sv
then
    svc -u test.sv
    while [ -e test.sv/run2 ]
    do
      sleep 1
    done
    svstat test.sv | filter_svstat
    svc -t test.sv
    until svstat test.sv | grep -q orphanage
    do
      sleep 1
    done
    svstat test.sv | filter_svstat
    cat test.sv/output
    svc -+t test.sv
    while [ -e test.sv/run3 ]
    do
	sleep 1
    done
    wait
    svstat test.sv/subtest | filter_svstat
    svc -tx test.sv/subtest
    while svok test.sv/subtest
    do
	sleep 1
    done
    cat test.sv/output
    cat test.sv/output2
else
    echo "This test fails on older Unix systems"
    echo "(everything which is not Linux or FreeBSD)"
    echo "as they have no subprocess reapers"
fi
rm test.sv/orphanage
