@ok
<?php

	$x = 10;
	$y = 20;

	if(isset($x))
	{
		echo "x is set\n";
	}
		  
	if(isset($x, $y))
	{
		echo "x, y set.\n";
	}

	$z = null;
	if(isset($z))
	{
		echo "z is set";
	}
	
?>
