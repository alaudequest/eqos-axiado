# Build và nạp từ VS Code

Mở `eth_4.code-workspace` bằng **File > Open Workspace from File**.

- **Ctrl+Shift+B**: build CM7 (mặc định).
- **Terminal > Run Task > STM32: Build and Flash CM7 (ST-LINK)**: build, nạp ELF, verify rồi reset board.
- **STM32: List ST-LINK probes**: liệt kê bộ nạp USB.
- **STM32: Build both cores**: build CM4 và CM7.
- **STM32: Build and Flash both cores (ST-LINK)**: build cả hai, nạp CM4 rồi CM7, verify và reset.

Kết nối cổng USB ST-LINK của board NUCLEO-H755ZI-Q. Đóng phiên debug đang giữ ST-LINK trước khi nạp. Nếu có nhiều bộ nạp, truyền `-SerialNumber` vào script hoặc thêm tham số đó trong `tasks.json`.

## Cách hoạt động

Các task gọi STM32CubeIDE ở chế độ dòng lệnh, dùng trực tiếp `.project` và `.cproject` hiện có. Cần giữ bản CubeIDE tại `D:\Stm32IDE\STM32CubeIDE_1.19.0\STM32CubeIDE`; nếu đổi vị trí, sửa giá trị `CubeIdePath` trong `.vscode/stm32.ps1`.

Workspace Eclipse riêng nằm trong `.vscode/.cubeide-workspace`. Không build đồng thời project này từ CubeIDE và VS Code vì chúng dùng chung thư mục `Debug`. Build lần đầu có thể mất thời gian để Eclipse khởi động và import project.

Firmware đầu ra:

- CM7: `CM7/Debug/eth_4_CM7.elf`.
- CM4: `CM4/Debug/eth_3_CM4.elf` (tên gốc trong project CubeIDE).

Lệnh Flash luôn build trước và dừng nếu build lỗi. Nạp qua SWD, connect under reset, giống cấu hình `.launch` hiện có. CM7 là mặc định theo cấu hình nạp cũ; dùng task cả hai lõi khi cần cập nhật firmware CM4.

IntelliSense có cấu hình CM7 và CM4 trong `.vscode/c_cpp_properties.json`. Chọn bằng **C/C++: Select IntelliSense Configuration**. Khi đổi include paths hoặc defines trong CubeIDE, cập nhật tương ứng file này.

Cấu hình này cung cấp build và nạp qua **Run Task**; chưa cấu hình debug bằng F5.
