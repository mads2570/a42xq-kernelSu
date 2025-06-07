```bash
#!/bin/bash

# Fix unsupported character in mkdtimg
sed -i 's/\'/\\\x27/g'
/home/runner/work/a42xq-kernelSu/a42xq-kernelSu/kernel-source//tools/mkdtimg
```