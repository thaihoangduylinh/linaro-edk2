# EmmcDump

Ứng dụng UEFI ARM 32-bit để đọc toàn bộ thiết bị nội bộ được firmware công bố qua
`EFI_BLOCK_IO_PROTOCOL`, từ LBA 0 đến `LastBlock`, và ghi bản dump lên USB.

## Build

Module đã được thêm vào `[Components.common]` của `QcomPkg/Msm8960Pkg/Msm8960Pkg.dsc`.
Mở Developer Command Prompt VS2015 và chạy như trước:

```bat
cd /d D:\Git\linaro-edk2\QcomPkg\Msm8960Pkg
b.bat
```

Với `b.bat` hiện tại (`RELEASE`, `GCC48`), file ứng dụng dự kiến ở:

```text
D:\Git\linaro-edk2\Build\QcomPkg\Msm8960Pkg\RELEASE_GCC48\ARM\EmmcDump.efi
```

Hoặc chỉ build module, sau khi setup môi trường và `GCC48_ARM_PREFIX`:

```bat
cd /d D:\Git\linaro-edk2
call edksetup.bat
build -p QcomPkg\Msm8960Pkg\Msm8960Pkg.dsc -m QcomPkg\Msm8960Pkg\Applications\EmmcDump\EmmcDump.inf -a ARM -t GCC48 -b RELEASE
```

Module không được thêm vào FDF. File `.efi` được sinh riêng; các lệnh tạo FD và
`recovery.img` trong `b.bat` vẫn là luồng build firmware hiện có.

## Chạy và chọn thiết bị

1. Chuẩn bị USB FAT32 còn trống ít nhất dung lượng thiết bị nguồn + 16 MiB.
   Chỉ cắm USB đích cần dùng. Nên dùng USB có dung lượng lớn hơn eMMC.
2. Dùng loader có khả năng thực thi ứng dụng UEFI ARM để nạp `EmmcDump.efi`.
   Nếu đã có UEFI Shell, có thể chạy `fs0:\EmmcDump.efi` (đổi `fs0:` cho đúng USB).
3. Ứng dụng bắt đầu tự động, không yêu cầu bàn phím. Nếu có console input, nhấn ESC
   để hủy giữa các lượt đọc. Mỗi lần chạy tạo thư mục mới `EmmcDump-0000`,
   `EmmcDump-0001`, ...; không ghi đè các bản dump trước.
4. Chờ báo thành công. Kiểm tra `manifest.txt` có dòng `COMPLETE`, rồi kiểm tra
   các file bằng script bên dưới trước khi dùng bản dump.

USB được nhận diện bằng node USB trong device path. Ưu tiên filesystem chứa chính
ứng dụng nếu đó là USB; nếu loader nạp từ RAM/NBH, yêu cầu duy nhất một filesystem
USB. USB có nhiều phân vùng filesystem cũng có thể tạo ra sự mơ hồ.

Nguồn phải có `MediaPresent=TRUE`, `LogicalPartition=FALSE`,
`RemovableMedia=FALSE`, device path hợp lệ, không phải USB và không cùng nhánh
thiết bị nguồn/đích. Ứng dụng yêu cầu duy nhất một ứng viên và ghi device path,
block size, LBA cuối, số byte vào manifest. **Đây là cách chọn ứng viên ổ nội bộ,
không phải phép xác nhận danh tính eMMC.** Kiểm tra thông tin đó đúng với máy.
Ứng dụng không chọn dựa trên block size lớn nhất hoặc dung lượng lớn nhất.

Nếu firmware chỉ công bố từng phân vùng hoặc dùng device path USB riêng không
chuẩn, ứng dụng dừng thay vì tự đoán. Firmware/loader phải cung cấp sẵn Block I/O
cho nguồn và Simple File System cho USB; ứng dụng không kèm USB/MMC driver và
không thể tự bổ sung các giao thức còn thiếu.

## Nội dung dump và xử lý lỗi

- Dung lượng = `(LastBlock + 1) * BlockSize`, tính bằng 64 bit có kiểm tra tràn.
- Chỉ gọi `ReadBlocks` trên nguồn. Không gọi `WriteBlocks`, format, reset ổ đĩa
  hoặc lệnh thay đổi phân vùng eMMC.
- Buffer mặc định 4 MiB, căn chỉnh theo `IoAlign`, giảm kích thước nếu thiếu RAM.
- Mỗi file tối đa 1 GiB, làm tròn xuống theo sector; phù hợp giới hạn file FAT32.
  File cuối chứa đúng phần còn lại. Nối theo số thứ tự sẽ có ảnh raw liên tục.
- Xử lý ghi thiếu byte, hết chỗ, media change, lỗi đọc/ghi/flush và hủy. Không tự
  bỏ qua sector lỗi hay chèn số 0. Không có resume: chạy lại tạo thư mục mới.
- Mỗi phần đã đóng/flush có số byte và CRC32 IEEE trong manifest. CRC được tính
  từ dữ liệu đọc vào RAM, không phải đọc kiểm tra lại USB trên điện thoại.
- Khi có lỗi, giữ file dở để chẩn đoán, ghi `INCOMPLETE`/`FAILED` nếu USB còn ghi
  được. Thiếu `COMPLETE` nghĩa là chưa có bản dump hoàn chỉnh.
- Watchdog boot được tắt để cho phép dump dài; nếu không tắt được, ứng dụng dừng.
  Sau khi kết thúc ứng dụng chờ 10 giây rồi trả quyền điều khiển cho loader.

"Toàn bộ" ở đây là toàn bộ vùng raw mà handle Block I/O đó cung cấp, thường là
**eMMC user area**, gồm bảng phân vùng và dữ liệu trong vùng này. Không tuyên bố
bao gồm boot0, boot1, RPMB, vùng ẩn hoặc phần NAND vật lý ngoài giao diện Block I/O.
Nếu firmware cung cấp nhiều vùng raw thì phiên bản này dừng vì có nhiều ứng viên.

## Kiểm tra và ghép trên PC

Dùng Python **3**, không dùng Python 2.7 của BaseTools:

```bat
py -3 verify_dump.py E:\EmmcDump-0000
py -3 verify_dump.py E:\EmmcDump-0000 --merge D:\Backups\emmc.bin
```

Script kiểm tra COMPLETE, geometry, thứ tự, số byte và CRC32 của tất cả các phần.
Đích ghép phải ở ngoài thư mục dump, trên filesystem hỗ trợ file lớn và hard link
(ví dụ NTFS/ext4). Nó tạo `emmc.bin.partial`, chỉ công bố tên `emmc.bin` sau khi
kiểm tra đầy đủ, và từ chối ghi đè file có sẵn. Nếu lỗi, `.partial` được giữ lại;
hãy chọn tên đích khác hoặc tự xử lý file dở trước khi thử lại.

## HTC NBH

Đầu ra của project này là `EmmcDump.efi`, chưa phải `ACWLDIAG.nbh`.
Không thể chỉ đổi đuôi `.efi` thành `.nbh`. Đóng gói lại NBH và việc bootloader chấp
nhận header/chữ ký của gói cần được xác minh riêng với loader HTC đang dùng.
`DIAG.efi` đã kiểm tra trên Desktop có PE machine `0x01C2` (ARM Thumb) và subsystem
10 (EFI application), phù hợp hướng target ARM; điều này chưa chứng minh ứng dụng
mới sẽ được bootloader chấp nhận hay firmware sẽ cung cấp đủ giao thức.

Chưa kiểm thử trên HTC 8X thật. Project được bàn giao source để bạn tự build.
