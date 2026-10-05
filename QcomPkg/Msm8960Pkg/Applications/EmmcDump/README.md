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
3. Phiên bản 1.3 hiển thị menu, chỉ bắt đầu dump khi bấm Power để chọn. Lần đầu
   làm theo hướng dẫn bấm/thả Volume Up, Volume Down, Power để nhận diện mã phím.
   Nếu có console input, nhấn ESC để hủy giữa các lượt đọc. Mỗi lần chạy tạo thư mục mới `EmmcDump-0000`,
   `EmmcDump-0001`, ...; không ghi đè các bản dump trước.
4. Chờ báo thành công. Kiểm tra `manifest.txt` có dòng `COMPLETE`, rồi kiểm tra
   các file bằng script bên dưới trước khi dùng bản dump.

## Menu và nút điện thoại (phiên bản 1.3)

Menu có bốn lựa chọn:

1. **FULL DUMP (1 GIB PARTS)**: toàn bộ eMMC User, chia file như trước.
2. **PARTITIONS FROM PARTITION.TXT**: chỉ các phân vùng GPT được chỉ định.
3. **GPT (PRIMARY + BACKUP)**: MBR và GPT chính ở đầu eMMC User, cùng GPT dự phòng ở cuối.
4. **EXIT**: thoát ứng dụng và trả quyền điều khiển về loader bằng `EFI_SUCCESS`.

Sau mỗi lượt dump thành công, hủy hoặc báo lỗi, ứng dụng hiển thị kết quả trong
5 giây rồi quay về menu chờ lệnh. Mỗi lượt tạo thư mục mới và mở lại thiết bị USB.
Ánh xạ phím được giữ trong RAM trong suốt phiên chạy. Lỗi khởi tạo menu/không có
giao thức nhập phím vẫn có thể khiến ứng dụng thoát về loader.

File `EmmcDump.keys` đã cung cấp có mã scan Volume Up `0x0001`, Volume Down
`0x0002`, Power `0x0102`; Unicode của cả ba bằng 0. Giữ file ở gốc USB để ứng dụng
nạp ánh xạ này, không cần học lại mỗi lần khởi động.

Volume Up đi lên, Volume Down đi xuống, Power chọn. Nhấn ngắn rồi thả từng nút.
Lần đầu ứng dụng học ba mã phím theo thứ tự trên và lưu vào `\EmmcDump.keys`
trên USB. Những lần sau vào thẳng menu. Để nhận diện lại (đổi loader hoặc bấm sai),
xóa `EmmcDump.keys` trên USB. Nếu không lưu được file này thì lần sau sẽ học lại.

Ứng dụng đọc cả Simple Text Input và Simple Text Input Ex, kể cả các handle chưa
được gắn vào `ConIn`. Không tự đọc thanh ghi GPIO/PMIC. Loader phải công bố sự kiện
của nút qua một trong hai giao thức này. Nếu màn hình học phím không nhận nút,
cần bổ sung giao thức keypad riêng của firmware; mã phím không được đoán từ GPIO.
Có thể dùng bàn phím USB qua hub và học ba phím thay thế nếu firmware hỗ trợ.
Menu chờ lựa chọn, không tự chuyển sang dump khi hết thời gian.

## Dump GPT

GPT không mặc định là 2 MiB đầu eMMC. Mục 3 đọc kích thước và vị trí từ hai GPT
header, kiểm tra CRC32 của header và bảng entry, kiểm tra hai bản có thông tin
nhất quán. GPT lỗi hoặc không nhất quán sẽ báo lỗi rồi quay lại menu; không tự sửa
GPT và không ghi vào eMMC. Mục này không dùng `partition.txt`.

Đầu ra nằm trong thư mục `EmmcDump-NNNN` mới:

```text
manifest.txt
p0000-GPT-primary/manifest.txt
p0000-GPT-primary/emmc-0000.bin
p0001-GPT-backup/manifest.txt
p0001-GPT-backup/emmc-0000.bin
```

Vùng chính bắt đầu tại LBA 0, bao gồm MBR, primary header và bảng entry chính.
Vùng dự phòng bắt đầu tại bảng entry dự phòng và kết thúc tại LBA cuối eMMC.
Manifest lưu LBA nguồn và số byte của từng vùng. Không nối hai vùng thành ảnh
toàn ổ. Nếu vùng metadata lớn hơn 1 GiB thì vẫn chia thành nhiều file như chế độ
full. Dung lượng USB cần bằng tổng hai vùng cộng 16 MiB dự phòng.

`verify_dump.py` nhận diện `mode=gpt` và kiểm tra cả hai thư mục con. Để tương
thích định dạng range dump đang có, manifest dùng các trường `partition_dir`,
`partition_name`, `partition_set`, `PARTITION_SET_COMPLETE` cho cả vùng GPT;
`Scope` trong manifest con ghi rõ đây là metadata GPT, không phải phân vùng.

## partition.txt

Copy file mẫu `partition.txt` cạnh source vào **thư mục gốc USB** và sửa tên phân
vùng theo GPT của máy. Một tên trên một dòng, ví dụ:

```text
# Only the partitions I need
SBL1
UEFI
MainOS
```

Đây chỉ là tên ví dụ, không bảo đảm máy nào cũng có. Tên được so sánh không phân
biệt hoa/thường; bỏ khoảng trắng đầu/cuối. File ASCII hoặc UTF-8 chứa tên ASCII
(chấp nhận UTF-8 BOM), không dùng UTF-16. Dòng trống và dòng bắt đầu bằng `#` được
bỏ qua. Giới hạn 64 tên, mỗi tên 36 ký tự, file tối đa 16 KiB. Không hỗ trợ LBA
tự nhập, ký tự đại diện hoặc danh sách ngăn cách bằng dấu phẩy.

Ứng dụng kiểm tra chữ ký, header CRC32 và entry-array CRC32 của GPT chính, giới
hạn LBA và các vùng đã chọn không chồng nhau. GPT chính lỗi thì dừng, chưa tự phục
hồi từ GPT dự phòng. Tất cả tên phải tồn tại duy nhất; tên sai/trùng sẽ dừng trước
khi đọc dump. Các tên và LBA thực tế trong GPT được ghi vào manifest để đối chiếu.

Dung lượng USB cần bằng tổng dung lượng **các phân vùng đã chọn** + 16 MiB. Phân
vùng có dung lượng lớn vẫn được chia thành các phần tối đa 1 GiB. Ví dụ đầu ra:

```text
EmmcDump-0001/
  manifest.txt
  p0000-SBL1/
    manifest.txt
    emmc-0000.bin
  p0001-UEFI/
    manifest.txt
    emmc-0000.bin
```

Manifest con ghi tên, LBA nguồn và CRC32 từng file. Trường `last_lba` trong geometry
con tính từ đầu ảnh phân vùng (LBA tương đối); `source_start_lba/source_end_lba` là
LBA thật trên eMMC. Manifest cha chỉ có `PARTITION_SET_COMPLETE` khi tất cả phân
vùng đã hoàn tất. Đây **không phải** ảnh full disk, không có GPT hoặc khoảng trống
ngoài các phân vùng. Không nối ảnh các phân vùng thành ảnh eMMC nguyên vẹn.

USB được nhận diện bằng node USB trong device path. Ưu tiên filesystem chứa chính
ứng dụng nếu đó là USB; nếu loader nạp từ RAM/NBH, yêu cầu duy nhất một filesystem
USB. USB có nhiều phân vùng filesystem cũng có thể tạo ra sự mơ hồ.

Nguồn phải có `MediaPresent=TRUE`, `LogicalPartition=FALSE`,
`RemovableMedia=FALSE`, device path hợp lệ, không phải USB và không cùng nhánh
thiết bị nguồn/đích. Phiên bản 1.1 ưu tiên đúng một handle raw có device path
`VenHw(B615F1F5-5088-43CD-809C-A16E52487D00)` của vùng eMMC User. GUID này cũng
có trong driver `QcomPkg/Msm8960Pkg/Dxe/MMCHSDxe/MMCHS.c` của repo.

Các path `12C55B20-25D3-41C9-8E06-282D94C676AD` (Boot 1),
`6B76A6DB-0257-48A9-AA99-F6B1655F7B00` (Boot 2) và
`C49551EA-D6BC-4966-9499-871E393133CD` (RPMB) được liệt kê trong manifest nhưng
không chọn làm vùng User. Tham khảo [bảng device path của img2ffu](https://github.com/MobileTooling/img2ffu#samples).
Nếu không tìm thấy GUID User, chỉ chấp nhận duy nhất một ứng viên raw còn lại;
không tự chọn theo dung lượng. Hai handle cùng GUID User cũng khiến ứng dụng dừng.
Thông tin device path, block size, LBA cuối và dung lượng được ghi để đối chiếu.

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
  Sau khi kết thúc ứng dụng chờ 30 giây rồi trả quyền điều khiển cho loader.

"Toàn bộ" ở đây là toàn bộ vùng raw mà handle Block I/O đó cung cấp, thường là
**eMMC user area**, gồm bảng phân vùng và dữ liệu trong vùng này. Không tuyên bố
bao gồm boot0, boot1, RPMB, vùng ẩn hoặc phần NAND vật lý ngoài giao diện Block I/O.
Các vùng Boot 1/Boot 2/RPMB không được ghép chung vào file ảnh vùng User.

## Log trên màn hình (phiên bản 1.1)

Ứng dụng tìm GOP (`EFI_GRAPHICS_OUTPUT_PROTOCOL`) và tự vẽ chữ bằng `Blt`,
bao gồm chế độ `PixelBltOnly`. Không cần font HII hoặc GraphicsConsoleDxe.
Log cuộn trong vùng chính, tiến độ nằm ở dòng cuối. Font ASCII nhỏ hiển thị
chữ hoa; nội dung `manifest.txt` vẫn giữ nguyên chữ hoa/chữ thường.

Nếu không tìm thấy GOP hoặc thao tác Blt thất bại, ứng dụng dùng `ConOut`.
Manifest ghi `display=GOP` hoặc `display=ConOut`, `gop_status`, `text_status`.
`ConOut` có thể là cổng serial hoặc console không nối màn hình, vì vậy trạng thái
thành công của nó không chứng minh người dùng nhìn thấy chữ. Nếu GOP cũng không
có, cần giao thức hiển thị riêng của firmware; ứng dụng chưa có driver riêng đó.

Tất cả số dùng `%d`/`%Ld` theo PrintLib cũ của repo; không dùng `%u`/`%Lu`.
Bản cũ tạo tên `EmmcDump-   u` và các trường `u` là do lỗi định dạng này.
Giữ thư mục cũ để chẩn đoán hoặc tự xóa nếu không cần; bản mới tạo tên đúng như
`EmmcDump-0000` và không ghi đè bản cũ.

## Kiểm tra và ghép trên PC

Dùng Python **3**, không dùng Python 2.7 của BaseTools:

```bat
py -3 verify_dump.py E:\EmmcDump-0000
py -3 verify_dump.py E:\EmmcDump-0000 --merge D:\Backups\emmc.bin
```

Với chế độ phân vùng, kiểm tra toàn bộ tập hoặc ghép riêng một phân vùng:

```bat
py -3 verify_dump.py E:\EmmcDump-0001
py -3 verify_dump.py E:\EmmcDump-0001\p0001-UEFI --merge D:\Backups\UEFI.bin
```

`--merge` trên thư mục cha của tập phân vùng bị từ chối để tránh tạo ảnh full disk
sai cấu trúc.

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
Phần menu/GPT của phiên bản 1.2 chưa build hoặc chạy kiểm thử, theo yêu cầu chỉ sửa source.

## Kiểm tra trên PC

Trong Developer Command Prompt VS2015 x86, từ thư mục repo:

```bat
QcomPkg\Msm8960Pkg\Applications\EmmcDump\tests\run-host-checks.bat
py -3 -m unittest discover -s QcomPkg\Msm8960Pkg\Applications\EmmcDump -v
```

Host checks dùng chính BasePrintLib trong repo để kiểm tra số 64-bit, tên file,
CRC32, nhận diện GUID User/Boot và mô phỏng GOP Blt để kiểm tra chữ, cuộn và
fallback. Chúng không kiểm chứng driver hiển thị hay eMMC trên điện thoại thật.
