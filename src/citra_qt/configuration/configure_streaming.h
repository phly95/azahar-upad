// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QWidget>

namespace Ui {
class ConfigureStreaming;
}

class ConfigureStreaming final : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureStreaming(QWidget* parent = nullptr);
    ~ConfigureStreaming() override;

    void ApplyConfiguration();
    void RetranslateUI();
    void SetConfiguration();

private:
    void UpdateCustomResEnabled();

    std::unique_ptr<Ui::ConfigureStreaming> ui;
};
