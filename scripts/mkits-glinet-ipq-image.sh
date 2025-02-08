#!/usr/bin/env bash
 
build_path="$1";
its_path="$build_path/flash.its";
scr_path="$build_path/flash.scr";
echo "its: $its_path";
echo "scr: $scr_path";
echo "prefix: $2";

# create the gl-b3000 uboot script
mk_script()
{
echo  "if test \"x\$verbose\" = \"x\"; then
    failedmsg=\'[failed]\'
else
    failedmsg='######################################## Failed'
fi

if test -n \$soc_hw_version; then
    if test \"\$soc_hw_version\" = \"20180100\" || test \"\$soc_hw_version\" = \"20180101\" ; then
        echo 'soc_hw_version : Validation success'
    else
        echo 'soc_hw_version : did not match, aborting upgrade'
        exit 1
    fi
else
    echo 'soc_hw_version : unknown, skipping validation'
fi

if test \"\$machid\" = \"8040004\" ; then
    echo 'machid : Validation success'
else
    echo 'machid : unknown, aborting upgrade'
    exit 1
fi

if test \"x\$verbose\" = \"x\"; then
    echo \\\c'Flashing ubi:                                          '
    setenv stdout nulldev
else
    echo '######################################## Flashing ubi: Started'
fi

failreason='error: failed on image extraction'
imxtract \$imgaddr ubi || setenv stdout serial && echo \"\$failedmsg\" && echo \"\$failreason\" && exit 1
failreason='error: failed on partition erase'
nand device 0 && nand erase 0x00800000 0x07800000 || setenv stdout serial && echo \"\$failedmsg\" && echo \"\$failreason\" && exit 1
failreason='error: failed on partition write'
nand write \$fileaddr 0x00800000 0x3520000 || setenv stdout serial && echo \"\$failedmsg\" && echo \"\$failreason\" && exit 1
if test \"x\$verbose\" = \"x\"; then
    setenv stdout serial
    echo '[ done ]'
    setenv stdout nulldev
    setenv stdout serial
else
    echo '######################################## Flashing ubi: Done'
fi

exit 0" > "$scr_path"
};

# create the custom dts uboot expects 
mk_its()
{
echo "/dts-v1/;

/ {
        description = \"Flashing nand 800 20000\";
        images {

                script {
                        description = \"GL.iNET UBOOT UPGRADE V2\";
                        data = /incbin/(\"./flash.scr\");
                        type = \"script\";
                        arch = \"ARM64\";
                        compression = \"none\";
                        hash1 { algo = \"crc32\"; };
                };

                ubi {
                        description = \"$2.img\";
                        data = /incbin/(\"$2.img\");
                        type = \"firmware\";
                        arch = \"ARM64\";
                        os = \"Linux\";
			load =<0x41080000>;
			entry =<0x41080000>;
                        compression = \"none\";
                        hash1 { algo = \"crc32\"; };
                };		
	};
};" > $its_path
};

mk_script
# ARGS: <file dir> <file name>
mk_its "$1" "$2"
