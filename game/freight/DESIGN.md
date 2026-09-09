# Design

The simulation behind [README.md](README.md). Everything here is state on
the fixed step, reflected, and therefore saveable, inspectable and
rewindable. Nothing here needs art.

## Two scales, said out loud

The drawn valley is about 1.4 km across; the railway it stands for is fifteen
kilometres. `kMapScale` converts, in one place. The calendar likewise runs
faster than the fixed step, and `Clock::rate()` converts, in one place.

Everything the *company* is priced in — kilometres, km/h, tonnes, litres,
euros — is real. Everything the *screen* is in is drawn. A speed is the only
quantity that crosses, and it crosses in exactly one function. Vehicle bodies
are the deliberate exception: they are drawn life-size so they can be told
apart, which makes them far too big for the map, the way a lorry is on a road
atlas.

## The one idea

**Work, not distance.** Every cost in this game comes from work actually
done against mass, grade and drag. Fuel is burnt by the joule. Brakes wear
by the joule they throw away. A route is expensive because of what it makes
the vehicle do, not because of how long it is on the map.

This is what makes the valley worth looking at, and it is one formula.

## Vehicles

A vehicle is a `VehicleDef` row plus per-instance state.

```
VehicleDef        tare mass, capacity (t and m³), body kind,
                  power (kW), tractive effort curve, max speed,
                  Davis coefficients A, B, C, adhesion mass,
                  fuel tank, idle burn, price, wear rates

Vehicle           def, position on the network, load, fuel,
                  condition per component, driver, odometer
```

Body kind decides what it can carry: flat, box, tipper, tank, container.
A cargo that wants a tanker cannot go in a box, and that is most of the
fleet planning.

### Motion

Per tick, for a vehicle or a whole train:

```
grade      = network.grade_at(edge, t)               // engine addition
R_roll     = A + B·v + C·v²                          // Davis, per tonne
R_grade    = m · g · grade
R_curve    = k / radius                              // rail only, small
F_max      = min(effort_curve(v), μ · m_adhesion · g)
a          = (F − R_roll − R_grade − R_curve) / (m · rotating_factor)
```

Consequences fall out of this and are not written anywhere else:

- A train too heavy for the grade has `a ≤ 0` and **stalls**. It does not
  explode; it stops, and you have to split it or send a banker.
- A lorry with the same load climbs the same hill slowly and burns the fuel
  to do it.
- Downhill, the same energy has to be got rid of, which is what brakes are
  for and why they wear.

Rolling stock is a list of wagons: masses add, drag coefficients add,
tractive effort does not. Coupling and uncoupling in the yard is the
shunter's job and takes time.

### Fuel

```
work      = Σ F · v · dt          over the run, positive tractive only
fuel      = work / (η · energy_density) + idle_burn · time
```

`η` sits around 0.35 for a diesel and is a per-def number, so an old lorry
is worse and you can feel it in the ledger.

### Wear

Condition per component, `0..1`, each worn by the thing that actually
wears it:

| Component | Worn by |
| --- | --- |
| Engine | tractive work done, worse under high load |
| Brakes | braking energy dissipated — so, descents |
| Tyres / wheels | distance × load, plus lateral acceleration in curves |
| Coupling, body | load cycles |

Below a threshold a component's performance drops; below a second one it
fails, and a failure is an event on the timeline with a location, not a
dice roll in a menu. Maintenance is bought before the threshold or paid for
after it, at a much worse price, in a lay-by.

## The network

Rail and road are **two `CurveNetwork`s**, meeting at the yard. The tidier
design — one network with a per-edge kind — does not work yet: a track
penalty belongs to the network, not to the vehicle asking, so there is no way
to say "impassable *for a lorry*" while a train routes over it happily.

Per-class routing costs are therefore on the engine list, and until they
exist two networks is what the engine actually supports. Nothing else in this
document changes when they arrive; the yard already owns one of each.

**Single track.** The branch is one track with passing loops. Two trains
meeting is a conflict at a node, which the engine already models; the game
supplies the rule about who waits.

**Main-line slots.** At the valley mouth, departures onto the main line
need a slot booked in advance. Miss it and the next one is hours away.
This is the pressure that makes the yard's clock matter.

## Cargo and contracts

```
CargoDef        name, density, body kinds accepted, handling time per tonne,
                whether it needs a ramp, crane, tipper or pump

Contract        cargo, tonnes, from, to, ready date, due date,
                rate per tonne, late penalty, recurring interval
```

A contract is finished when the tonnes are delivered. Partial delivery over
several runs is normal and is the point: five wagons or three trips.

Handling time is real time at both ends, and it needs the equipment. A
tonne of gravel into a tipper is fast; a tonne of pallets without a
forklift is the afternoon.

**Reputation** rises with on-time completion and falls with lateness. It
gates the size of contract offered. It is one number and it does not need
to be more.

## Money

A ledger, monthly close.

| In | Out |
| --- | --- |
| Contract payments | Diesel (wholesale) |
| Pump sales | Wages, per driver, per month |
| Scrap | Maintenance and parts |
| | Depot rent and rates |
| | Loan interest — grandfather's, and any you take |

Starting position: the yard, two lorries, one shunter, a handful of wagons,
and a loan that costs more per month than one contract pays. The first hour
of the game is about not going under, not about growing.

Diesel has a wholesale price that moves weekly on a seeded random walk with
a slow trend. It is the same price your lorries pay and the basis of what
you charge at the pump, so one number ties the whole company together.

## The filling station

At the gate, on the public road.

```
volume = base_traffic · elasticity_curve(your_price / market_price) · hour_factor
margin = your_price − wholesale_price
```

Price high and the margin is good and the cars go on to the next town.
Price low and the forecourt is busy and you are working for nothing. The
interesting bit is the coupling: the tank is shared with your fleet, so a
busy week at the pump means buying diesel at the wrong moment for the
lorries.

The shop is a second, smaller version of the same curve, with a much better
margin, which is how filling stations actually work.

## Time

One game hour is ninety simulated seconds, so a day is thirty-six minutes at
normal speed and nine at 4×. That ratio is not arbitrary: it is the one where
a lorry's round trip reads as a morning's work and still only takes a couple
of minutes to watch. A calendar with dates and weekdays drives deadlines,
wages, the fuel price and the rhythm of the road.

Anything measured in hours — handling, driver shifts, opening times — is
game time, and divides by `Clock::rate()` to become simulated seconds. Only
motion is in simulated seconds directly, because motion is what you watch.

**Driver hours,** roughly to European rules because they make a good
puzzle: nine hours driving a day, a forty-five minute break after four and
a half, eleven hours rest between shifts. A driver out of hours stops where
they are. Two drivers on one lorry is a way to buy distance, and it costs.

## Failure, and how it reads

Nothing in this game kills you suddenly. The failures are all slow and all
legible after the fact, which is what the timeline is for:

- Stalled on a grade — too much load for the loco.
- Out of fuel — the tank was low because the pump had a good week.
- Brake failure on a descent — worn on the last three descents.
- Driver out of hours — the schedule never had the slack.
- Missed main-line slot — the handling took longer than the plan allowed.
- Late delivery — any of the above.

When a run comes in late, the player rewinds it and watches. That is the
feature no other game in this genre has, and it is free here.

## What the engine has to grow

Each one generic, each one useful to any game.

1. **`core/ease.h`** — easing functions and a reflected `Tweened<T>` that
   moves toward a target over a duration. Needed for every animated value
   in [STYLE.md](STYLE.md).
2. **`sim/clock.h`** — a game calendar derived from the tick: date, time of
   day, weekday, with a configurable scale. Reflected.
3. **`grade_at(edge, t)`** on `CurveNetwork` — the derivative of the
   elevation the network already stores.
4. **A resistance hook on `NetworkAgents`** — today an agent accelerates at
   a fixed rate. It should be able to ask the game for available force and
   resisting force and integrate that instead. Trains, lorries, boats,
   anything.
5. **UI II in `render/ui.h`** — scrolling lists, tables with sortable
   columns, tabs, text and number fields, and a line chart. A management
   game is mostly this.
6. **Formatting in `core/localization.h`** — currency, dates, and units
   (t, km, kW, l), localised.
7. **2D lighting and post-processing** — already on the engine's later
   list; phase 5 is when it is needed.
8. **Per-class routing costs** on `CurveNetwork` — a cost that depends on
   who is asking, so rail and road can share one network. Wanted by phase 6,
   when the valley has junctions worth sharing; until then the game keeps
   two networks.
