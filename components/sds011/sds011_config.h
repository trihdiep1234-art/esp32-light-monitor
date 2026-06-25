/** @file sds011_config.h
 * SDS011 configuration file. Allows setting default parameters and some
 * internal values.
 */

/** Outgoing (TX) packet queue size. */
#define SDS011_TX_QUEUE_SIZE 5

/** Incoming (RX) command response queue size. */
#define SDS011_RX_CMD_QUEUE_SIZE 5

// [SỬA ĐỔI 1]: Tăng kích thước hàng đợi chứa dữ liệu bụi mịn lên 5 
// để tránh bị tràn và mất gói tin khi lệch pha chu kỳ 1 giây của app_main
#define SDS011_RX_DATA_QUEUE_SIZE 5

/** TX buffer size (multiples of 1024).  */
#define SDS011_UART_TX_BUFFER_SIZE (1024 * 1)
/** RX buffer size (multiples of 1024). */
#define SDS011_UART_RX_BUFFER_SIZE (1024 * 1)

/** Name of the task processing the UART data. */
#define SDS011_TX_TASK_NAME "sds011tx"
#define SDS011_RX_TASK_NAME "sds011rx"

// [SỬA ĐỔI 2]: Tăng Stack Depth lên 4096 để cấp đủ bộ nhớ cho Task 
// không bị crash (Stack Overflow) khi xử lý chuỗi in Log Debug
#define SDS011_TX_TASK_STACK_DEPTH 4096
#define SDS011_RX_TASK_STACK_DEPTH 4096

// [SỬA ĐỔI 3]: Mở khóa dòng này (Xóa dấu //) để ép thư viện 
// tự in tiến trình quét gói tin thô ra màn hình Monitor
//#define SDS011_DEBUG