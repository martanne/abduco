#!/bin/sh

ABDUCO="./abduco"
# set detach key explicitly in case it was changed in config.h
ABDUCO_OPTS="-e ^\\"

[ ! -z "$1" ] && ABDUCO="$1"
[ ! -x "$ABDUCO" ] && echo "usage: $0 /path/to/abduco" && exit 1

TESTS_OK=0
TESTS_RUN=0

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
	dvtm_cmd 'qq'
}

expected_abduco_prolog() {
	printf "[?1049h[H"
}

# $1 => session-name, $2 => exit status
expected_abduco_epilog() {
	echo "[?25h[?1049labduco: $1: session terminated with exit status $2"
}

# $1 => session-name, $2 => cmd to run
expected_abduco_attached_output() {
	expected_abduco_prolog
	$2
	expected_abduco_epilog "$1" $?
}

# $1 => session-name, $2 => cmd to run
expected_abduco_detached_output() {
	expected_abduco_prolog
	$2 >/dev/null 2>&1
	expected_abduco_epilog "$1" $?
}

check_environment() {
	[ "`$ABDUCO | wc -l`" -gt 1 ] && echo Abduco session exists && exit 1;
	pgrep abduco && echo Abduco process exists && exit 1;
	return 0;
}

test_non_existing_command() {
	check_environment || return 1;
	$ABDUCO -c test ./non-existing-command >/dev/null 2>&1
	check_environment || return 1;
}

# $1 => session-name, $2 => command to execute
run_test_attached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test attached: $name "
	expected_abduco_attached_output "$name" "$cmd" > "$output_expected" 2>&1

	if $ABDUCO -c "$name" $cmd 2>&1 | sed 's/.$//' > "$output" && sleep 1 &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
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

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test detached: $name "
	expected_abduco_detached_output "$name" "$cmd" > "$output_expected" 2>&1

	if $ABDUCO -n "$name" $cmd >/dev/null 2>&1 && sleep 1 &&
	   $ABDUCO -a "$name" 2>&1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
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

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test: $name "
	$cmd >/dev/null 2>&1
	expected_abduco_epilog "$name" $? > "$output_expected" 2>&1

	if detach | $ABDUCO $ABDUCO_OPTS -c "$name" $cmd >/dev/null 2>&1 && sleep 3 &&
	   $ABDUCO -a "$name" 2>&1 | tail -1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

run_test_dvtm() {
	echo -n "Running dvtm test: "
	if ! which dvtm >/dev/null 2>&1; then
		echo "SKIPPED"
		return 0;
	fi

	TESTS_RUN=$((TESTS_RUN + 1))
	local name="dvtm"
	local output="$name.out"
	local output_expected="$name.expected"

	: > "$output_expected"
	if dvtm_session | $ABDUCO -c "$name" > "$output" 2>&1 &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

run_test_detect_session() {
	check_environment || return 1;

	local name="$1"
    local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test: $name "
	$cmd >/dev/null 2>&1
	expected_abduco_epilog "$name" $? > "$output_expected" 2>&1

	if detach | $ABDUCO $ABDUCO_OPTS -c "$name" $cmd >/dev/null 2>&1 && sleep 3 &&
       $ABDUCO -d "$name" &&
	   $ABDUCO -a "$name" 2>&1 | tail -1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

test_non_existing_command || echo "Execution of non existing command FAILED"

run_test_attached "awk" "awk 'BEGIN {for(i=1;i<=1000;i++) print i}'"
run_test_detached "awk" "awk 'BEGIN {for(i=1;i<=1000;i++) print i}'"

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
run_test_detect_session "detect-session" "./long-running.sh"

rm ./long-running.sh

run_test_dvtm

[ $TESTS_OK -eq $TESTS_RUN ]
