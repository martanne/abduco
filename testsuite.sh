#!/bin/bash

ABDUCO="./abduco"

detach() {
	sleep 1
	printf ""
}

dvtm_cmd() {
	printf "$1\n"
	sleep 1
}

dvtm_session() {
	sleep 1
	dvtm_cmd 'c'
	dvtm_cmd 'c'
	dvtm_cmd 'c'
	sleep 1
	dvtm_cmd ' '
	dvtm_cmd ' '
	dvtm_cmd ' '
	sleep 1
	dvtm_cmd 'q'
}

# $1 => session-name, $2 => exit status
expected_abduco_output() {
	echo "[?25h[999Habduco: $1: session terminated with exit status $2"
}

check_environment() {
	[ "`$ABDUCO | wc -l`" -gt 1 ] && echo Abduco session exists && exit 1;
	pgrep abduco && echo Abduco process exists && exit 1;
	return 0;
}

test_non_existing_command() {
	check_environment || return 1;
	$ABDUCO -c test ./non-existing-command &> /dev/null
	check_environment || return 1;
}

# $1 => session-name, $2 => command to execute
run_test_attached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	echo -n "Running test attached: $name "
	$cmd &> "$output_expected"
	expected_abduco_output "$name" $? >> "$output_expected"
	$ABDUCO -c "$name" $cmd 2>&1 | head -n -1 | sed 's/.$//' > "$output"
	if diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

# $1 => session-name, $2 => command to execute
run_test_detached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	echo -n "Running test detached: $name "
	$cmd &> /dev/null
	expected_abduco_output "$name" $? > "$output_expected"

	if $ABDUCO -n "$name" $cmd &> /dev/null && sleep 1 &&
	   $ABDUCO -a "$name" 2>&1 | head -n -1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

# $1 => session-name, $2 => command to execute
run_test_attached_detached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	echo -n "Running test: $name "
	$cmd &> /dev/null
	expected_abduco_output "$name" $? > "$output_expected"

	if detach | $ABDUCO -c "$name" $cmd &> /dev/null && sleep 3 &&
	   $ABDUCO -a "$name" 2>&1 | head -n -1 | tail -1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

run_test_dvtm() {
	echo -n "Running dvtm test: "
	if ! which dvtm &> /dev/null; then
		echo "SKIPPED"
		return 0;
	fi

	local name="dvtm"
	local output="$name.out"
	local output_expected="$name.expected"

	echo exit | dvtm &> /dev/null
	expected_abduco_output "$name" $? > "$output_expected"
	local len=`wc -c "$output_expected"  | awk '{ print $1 }'`
	len=$((len+1))
	if dvtm_session | $ABDUCO -c "$name" 2>&1 | head -n -1 | tail -c $len | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

test_non_existing_command || echo "Execution of non existing command FAILED"

run_test_attached "seq" "seq 1 1000"
run_test_detached "seq" "seq 1 1000"

run_test_attached "false" "false"
run_test_detached "false" "false"

run_test_attached "true" "true"
run_test_detached "true" "true"

cat > exit-status.sh <<-EOT
	#!/bin/sh
	exit 42
EOT
chmod +x exit-status.sh

run_test_attached "exit-status" "./exit-status.sh"
run_test_detached "exit-status" "./exit-status.sh"

rm ./exit-status.sh

cat > long-running.sh <<-EOT
	#!/bin/sh
	echo Start
	date
	sleep 3
	echo Hello World
	sleep 3
	echo End
	date
	exit 1
EOT
chmod +x long-running.sh

run_test_attached_detached "attach-detach" "./long-running.sh"

rm ./long-running.sh

run_test_dvtm
