// 文件类型标识符
#define T_DIR     1   // Directory // 目录标识符
#define T_FILE    2   // File // 文件标识符
#define T_DEVICE  3   // Device // 设备标识符

struct stat {
  int dev;     // File system's disk device // 磁盘设备
  uint ino;    // Inode number // inode序号
  short type;  // Type of file // 文件类型
  short nlink; // Number of links to file // 文件的链接数量
  uint64 size; // Size of file in bytes //文件大小（bytes）
};

