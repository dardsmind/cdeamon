<?php

if(!file_exists("/home/logs")) {
		shell_exec("mkdir /home/logs");
		shell_exec("chmod 777 /home/logs");  
}	
$myFile = "home/logs/command.log";
exec("touch $myFile");
$fh = fopen($myFile, 'a') or die("can't open file $myFile");
  $d = join("|",$argv);
	$stringData = "\n-----------------------------------------------------------------------\n";
	$stringData .= "$d\n";
	foreach($argv as $key=>$value){
	  $stringData .= "$key------ $value\n";
	}
fwrite($fh, $stringData);
fclose($fh);
