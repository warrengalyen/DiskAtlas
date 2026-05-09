#ifndef DA_DEFAULT_MIME_CATEGORY_SEEDS_H
#define DA_DEFAULT_MIME_CATEGORY_SEEDS_H

/*
 * Built-in MIME category defaults (extensions + colors).
 * Include only from da_default_mime_categories.c after defining:
 *   #define DA_DEFAULT_MIME_CATEGORY_SEEDS_DEFINE
 * Requires da_default_mime_categories.h for DaDefaultMimeCategorySeed.
 */

#ifdef DA_DEFAULT_MIME_CATEGORY_SEEDS_DEFINE

#define DA_DEFAULT_INS_AUDIO                                                                                         \
  ".aac\n.ac3\n.aif\n.aiff\n.amr\n.ape\n.au\n.caf\n.dff\n.dsf\n.dts\n.eac3\n.flac\n.gsm\n.m4a\n.m4b\n.mka\n.mid\n"   \
  ".midi\n.mp3\n.mpa\n.mpc\n.ogg\n.oga\n.opus\n.ra\n.ram\n.rf64\n.snd\n.tta\n.voc\n.wav\n.wma\n.wv\n.aax\n"

#define DA_DEFAULT_INS_VIDEO                                                                                         \
  ".3g2\n.3gp\n.3gpp\n.asf\n.avi\n.divx\n.f4v\n.flv\n.m2ts\n.m2v\n.m4v\n.mkv\n.mov\n.mp4\n.mpeg\n.mpg\n.mpv\n"      \
  ".mts\n.mxf\n.ogv\n.qt\n.rm\n.rmvb\n.ts\n.vob\n.webm\n.wmv\n.xvid\n.y4m\n"

#define DA_DEFAULT_INS_IMAGES                                                                                        \
  ".3fr\n.arw\n.bmp\n.cr2\n.cr3\n.dcr\n.dng\n.erf\n.exr\n.gif\n.hdr\n.heic\n.heif\n.ico\n.iiq\n.j2c\n.j2k\n.jp2\n"  \
  ".jpeg\n.jpg\n.jpf\n.jpm\n.jpx\n.kdc\n.mef\n.mos\n.mrw\n.nef\n.nrw\n.orf\n.pef\n.png\n.ppm\n.psd\n.raf\n.raw\n"   \
  ".rw2\n.sr2\n.srf\n.srw\n.svg\n.tga\n.tif\n.tiff\n.webp\n.x3f\n.xcf\n.pcx\n"

#define DA_DEFAULT_INS_COMPRESSED                                                                                    \
  ".7z\n.ace\n.alz\n.apk\n.arc\n.arj\n.br\n.bz2\n.cab\n.cbz\n.cpio\n.deb\n.ear\n.egg\n.gz\n.jar\n.lz\n.lz4\n.lzma\n" \
  ".lzo\n.pea\n.rar\n.rpm\n.s7z\n.sar\n.sit\n.sitx\n.sz\n.tar\n.tbz2\n.tgz\n.tlz\n.txz\n.war\n.whl\n.xz\n.zip\n"     \
  ".zst\n.zstd\n.crx\n"

#define DA_DEFAULT_INS_EXECUTABLES                                                                                   \
  "*.app\n*.AppImage\n.bat\n.cmd\n.com\n.command\n.cpl\n.deb\n.dll\n.dmg\n.dylib\n.exe\n.jar\n.msi\n.msp\n"         \
  ".msu\n.out\n.pif\n.pkg\n.ps1\n.reg\n.rpm\n.run\n.scpt\n.scr\n.sh\n.so\n.vbs\n.workflow\n.wsf\n.wsh\n.xpi\n"

#define DA_DEFAULT_INS_DOCUMENTS                                                                                     \
  ".123\n.602\n.abw\n.awt\n.chm\n.csv\n.dbf\n.doc\n.docb\n.docm\n.docx\n.dot\n.dotm\n.dotx\n.djvu\n.epub\n.fb2\n"   \
  ".hlp\n.hwp\n.key\n.lit\n.mdb\n.mobi\n.numbers\n.odm\n.odp\n.ods\n.odt\n.otp\n.ots\n.ott\n.pages\n.pdf\n.pot\n"   \
  ".potm\n.potx\n.pps\n.ppsm\n.ppsx\n.ppt\n.pptm\n.pptx\n.prc\n.rtf\n.sxw\n.uop\n.vsd\n.vsdx\n.wps\n.xlr\n.xls\n"   \
  ".xlsb\n.xlsm\n.xlsx\n.xlt\n.xltm\n.xltx\n.xps\n"

#define DA_DEFAULT_INS_DISK_IMAGES                                                                                   \
  ".adf\n.bin\n.cdi\n.cue\n.d64\n.dmg\n.fd\n.fdi\n.gdi\n.hdd\n.img\n.iso\n.mdx\n.mdf\n.mds\n.miniso\n.nrg\n.raw\n"  \
  ".sdi\n.sparseimage\n.toast\n.vcd\n.vdi\n.vfd\n.vhd\n.vhdx\n.vmdk\n.wim\n.swm\n.esd\n.qcow\n.qcow2\n"

#define DA_DEFAULT_INS_PLAIN_TEXT                                                                                    \
  ".adoc\n.asciidoc\n.asm\n.asp\n.aspx\n.bash\n.c\n.cc\n.cfg\n.clang-format\n.clang-tidy\n.cmake\n.cnf\n.conf\n"     \
  ".config\n.cpp\n.cs\n.css\n.cxx\n.def\n.diff\n.dockerignore\n.env\n.envrc\n.gitattributes\n.gitignore\n.gradle\n" \
  ".go\n.groovy\n.h\n.hpp\n.hs\n.html\n.htm\n.ini\n.java\n.jl\n.js\n.json\n.jsonl\n.kt\n.kts\n.less\n.lua\n.m\n"   \
  ".markdown\n.md\n.mjs\n.mm\n.patch\n.php\n.pl\n.pm\n.plist\n.properties\n.psm1\n.py\n.pyi\n.pyw\n.r\n.rb\n.rs\n"  \
  ".rst\n.sass\n.scala\n.scss\n.sql\n.svelte\n.swift\n.textile\n.toml\n.ts\n.tsx\n.txt\n.vb\n.vue\n.xml\n.xsd\n"    \
  ".xsl\n.xslt\n.yaml\n.yml\n.zsh\n.log\n.nfo\n.tf\n.tfvars\n.hcl\n.wsdl\n.avsc\n"

#define DA_DEFAULT_SENS_PLAIN_TEXT                                                                                   \
  "CMakeLists.txt\nDockerfile\nGNUmakefile\nJenkinsfile\nMakefile\nProcfile\nRakefile\nREADME\nREADME.md\n"          \
  "Vagrantfile\nAUTHORS\nCHANGELOG\nCONTRIBUTING\nCOPYING\nINSTALL\nLICENSE\nLICENCE\nTODO\n.editorconfig\n"

static const DaDefaultMimeCategorySeed da_default_mime_category_seeds_table[] = {
    {"Audio", "#9A510C", DA_DEFAULT_INS_AUDIO, ""},
    {"Video", "#8C8E04", DA_DEFAULT_INS_VIDEO, ""},
    {"Images", "#167620", DA_DEFAULT_INS_IMAGES, ""},
    {"Compressed Files", "#6D28A5", DA_DEFAULT_INS_COMPRESSED, ""},
    {"Executables", "#87264D", DA_DEFAULT_INS_EXECUTABLES, ""},
    {"Documents", "#0F9E7E", DA_DEFAULT_INS_DOCUMENTS, ""},
    {"Disk Images", "#4C681C", DA_DEFAULT_INS_DISK_IMAGES, ""},
    {"Plain Text", "#A31D96", DA_DEFAULT_INS_PLAIN_TEXT, DA_DEFAULT_SENS_PLAIN_TEXT},
};

#endif /* DA_DEFAULT_MIME_CATEGORY_SEEDS_DEFINE */

#endif /* DA_DEFAULT_MIME_CATEGORY_SEEDS_H */
