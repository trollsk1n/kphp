@kphp_should_fail
<?php
require_once 'kphp_tester_include.php';

function demo() {
    $t = tuple(1, 'str');
    $t['strkey'] = 'v';
}

demo();
