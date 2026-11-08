-- Ducato bench dashboard protocol v1. No changes to BeamNG core files.
local M = {}
local function on(v) return v == true or (type(v) == 'number' and v > 0) end
local function num(v) return type(v) == 'number' and v or 0 end
local function add(mask, value, bit) return on(value) and mask + bit or mask end

-- Mechanical failures are not reliably reflected in electrics.checkengine.
local damageNames = {
  'engineLockedUp', 'engineHydrolocked', 'catastrophicOverrevDamage',
  'catastrophicOverTorqueDamage', 'impactDamage', 'headGasketDamaged',
  'pistonRingsDamaged', 'rodBearingsDamaged', 'blockMelted',
  'cylinderWallsMelted', 'oilpanLeak', 'oilRadiatorLeak',
  'coolantOverheating', 'radiatorLeak', 'oilOverheating',
  'mildOverrevDamage', 'mildOverTorqueDamage', 'exhaustBroken'
}
local function engineFault(e)
  if on(e.checkengine) then return true end
  if damageTracker and damageTracker.getDamage then
    for _, name in ipairs(damageNames) do
      if on(damageTracker.getDamage('engine', name)) then return true end
    end
  end
  if powertrain and powertrain.getDevicesByCategory then
    for _, device in pairs(powertrain.getDevicesByCategory('engine') or {}) do
      if device.isBroken then return true end
    end
  end
  return false
end
local function oilFault(e)
  if on(e.oil) then return true end
  if damageTracker and damageTracker.getDamage then
    for _, name in ipairs({'starvedOfOil', 'oilLevelCritical', 'oilpanLeak',
                            'oilRadiatorLeak', 'rodBearingsDamaged', 'oilOverheating'}) do
      if on(damageTracker.getDamage('engine', name)) then return true end
    end
  end
  return false
end
local function oilCritical()
  if not damageTracker or not damageTracker.getDamage then return false end
  return on(damageTracker.getDamage('engine', 'starvedOfOil'))
      or on(damageTracker.getDamage('engine', 'oilLevelCritical'))
      or on(damageTracker.getDamage('engine', 'rodBearingsDamaged'))
end
M.getAddress = function() return '127.0.0.1' end
M.getPort = function() return 4568 end
M.getMaxUpdateRate = function() return 20 end
M.isPhysicsStepUsed = function() return false end
M.getStructDefinition = function()
  return [[
    char magic[8];
    float speed;
    float rpm;
    float fuel;
    float temperature;
    unsigned lights;
    unsigned flags;
  ]]
end

M.fillStruct = function(o, dtSim)
  local e = electrics.values
  if not e.watertemp then return end
  o.magic = 'DUCATO1'
  o.speed = num(e.wheelspeed or e.airspeed)
  o.rpm = num(e.rpm)
  o.fuel = math.max(0, math.min(1, num(e.fuel)))
  o.temperature = num(e.watertemp)
  local lights = 0
  lights = add(lights, e.lowbeam, 1)
  lights = add(lights, e.highbeam, 2)
  lights = add(lights, num(e.lights_state) > 0 or on(e.parking), 4)
  lights = add(lights, e.signal_L, 8)
  lights = add(lights, e.signal_R, 16)
  o.lights = lights
  local flags = 0
  flags = add(flags, num(e.ignitionLevel) >= 2, 1)
  flags = add(flags, e.engineRunning, 2)
  flags = add(flags, e.cruiseControlActive, 4)
  flags = add(flags, oilFault(e) and not oilCritical(), 8)
  flags = add(flags, num(e.engineRunning) == 0, 16)
  flags = add(flags, e.parkingbrake, 32)
  flags = add(flags, e.absActive, 64)
  local doorOpen = false
  for name, value in pairs(e) do
    if type(name) == 'string' and name:lower():find('door', 1, true)
       and name:sub(-12) == '_notAttached' and on(value) then doorOpen = true end
  end
  flags = add(flags, doorOpen, 128)
  flags = add(flags, on(e.lowfuel) or num(e.fuel) <= 0.1, 256)
  -- Optional warnings only when the vehicle actually publishes these fields.
  flags = add(flags, e.seatbeltWarning, 512)
  flags = add(flags, engineFault(e), 1024)
  flags = add(flags, e.dpfWarning, 2048)
  flags = add(flags, e.waterInFuelWarning, 4096)
  flags = add(flags, e.brakePadWarning, 8192)
  flags = add(flags, e.glowPlugFault, 16384)
  flags = add(flags, oilCritical(), 32768) -- Native cluster blinking oil warning.
  flags = add(flags, e.airbagWarning, 65536) -- Optional vehicle-provided SRS fault.
  flags = add(flags, e.airbagWarningBlink, 131072) -- Verified cluster fast blink.
  -- Two front-fog bits exist in this cluster; use the verified front-fog bit.
  o.lights = add(lights, e.fog, 32)
  o.flags = flags
end
return M
