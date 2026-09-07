#pragma once

struct EVTelemetry;

class IDataSource {
public:
    virtual ~IDataSource() = default;
    virtual void Update(float deltaTime) = 0;
    virtual bool IsConnected() const = 0;
    virtual const EVTelemetry* GetEVTelemetry() const = 0;
    virtual const char* GetSourceName() const = 0;
};
