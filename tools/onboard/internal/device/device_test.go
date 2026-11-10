package device

import "testing"

func TestChipName(t *testing.T) {
	for in, want := range map[string]string{
		"esp32s3": "ESP32-S3", "esp32c6": "ESP32-C6", "esp32": "ESP32",
		"unknown": "Board", "": "Board", "host": "HOST",
	} {
		if got := ChipName(in); got != want {
			t.Errorf("ChipName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestDescribe(t *testing.T) {
	b := &Board{}
	b.Device.Chip, b.Device.Firmware, b.Device.Version = "esp32s3", "zephyr", "0.0.0"
	b.Info.DeviceID, b.Info.State = "tedge-d405927b33d0", "waiting-network"
	if got := b.Describe(); got != "ESP32-S3 tedge-d405927b33d0, not on Wi-Fi yet" {
		t.Errorf("got %q", got)
	}
	b.Device.Firmware, b.Device.Version = "modbus-server", "0.10.0"
	b.Info.State, b.Info.Tenant = "connected", "example.cumulocity.com"
	if got := b.Describe(); got != "ESP32-S3 running modbus-server 0.10.0, connected to example.cumulocity.com" {
		t.Errorf("got %q", got)
	}
}
