@kphp_should_fail
<?php
require_once 'Classes/autoload.php';

function test_access_private_field() {
    $c = new Classes\C;
    var_dump($c->c2);
}

test_access_private_field();

