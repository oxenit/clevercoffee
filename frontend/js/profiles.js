// Brew profile editor, talks to GET/POST /profiles (format in src/utils/pressureProfileStorage.h).
// The stored profiles use the GaggiMate field names, the editor works on a flattened copy of each phase.

const CHART_WIDTH = 600
const CHART_HEIGHT = 120
const CHART_MAX_BAR = 12
const NAME_LENGTH = 23
const TRANSITIONS = ['instant', 'linear', 'ease-in', 'ease-out', 'ease-in-out']

function newPhase(name, mode, value, duration) {
    return { name: name, mode: mode, value: value, duration: duration, transition: 'linear', transitionDuration: 2, targets: [] }
}

// same curves as profileTransitionCurve() in the firmware
function transitionCurve(type, t) {
    if (t <= 0) return type === 'instant' ? 1 : 0
    if (t >= 1) return 1

    switch (type) {
        case 'instant': return 1
        case 'ease-in': return t * t
        case 'ease-out': return 1 - (1 - t) * (1 - t)
        case 'ease-in-out': return t < 0.5 ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t)
        default: return t
    }
}

/**
 * GaggiMate style profile -> editor profile. Unsupported features are dropped and reported in warnings,
 * the firmware would reject them (flow targets, volume based ramps, other exit target types).
 */
function fromStored(profile, warnings) {
    const label = String(profile.label || profile.name || 'Imported').substring(0, NAME_LENGTH)
    const phases = (profile.phases || []).map((phase, index) => {
        const where = label + ' / ' + (phase.name || 'phase ' + (index + 1))
        const edited = newPhase(String(phase.name || '').substring(0, NAME_LENGTH), 'pressure', 0, Number(phase.duration) || 0)

        if (typeof phase.pump === 'number') {
            edited.mode = 'power'
            edited.value = phase.pump
        } else if (phase.pump && phase.pump.target === 'flow') {
            edited.mode = 'pressure'
            edited.value = phase.pump.pressure > 0 ? phase.pump.pressure : 9
            warnings.push(where + ': flow target replaced by its pressure limit (' + edited.value + ' bar)')
        } else if (phase.pump) {
            edited.value = phase.pump.pressure >= 0 ? phase.pump.pressure : 0
            if (phase.pump.pressure < 0) warnings.push(where + ': "hold current pressure" (-1) is not supported, set to 0 bar')
        }

        const transition = phase.transition || { type: 'instant', duration: 0 }
        edited.transition = TRANSITIONS.includes(transition.type) ? transition.type : 'instant'
        edited.transitionDuration = Number(transition.duration) || 0

        if (transition.target && transition.target !== 'time') {
            warnings.push(where + ': ramp over ' + transition.target + ' converted to a ramp over the whole phase time')
            edited.transitionDuration = 0
        }

        if (transition.adaptive) warnings.push(where + ': adaptive transition not supported, ramps from the previous phase')
        if (phase.valve === 0) warnings.push(where + ': closed valve not supported, the brew valve stays open')

        for (const target of phase.targets || []) {
            if (target.type === 'volumetric' || target.type === 'pressure') {
                edited.targets.push({ type: target.type, operator: target.operator === 'lte' ? 'lte' : 'gte', value: Number(target.value) || 0 })
            } else {
                warnings.push(where + ': exit target "' + target.type + '" not supported, dropped')
            }
        }

        return edited
    })

    return { label: label, phases: phases }
}

function toStored(profile) {
    return {
        label: profile.label,
        type: 'pro',
        phases: profile.phases.map(phase => ({
            name: phase.name,
            duration: phase.duration,
            pump: phase.mode === 'power' ? phase.value : { target: 'pressure', pressure: phase.value },
            transition: { type: phase.transition, duration: phase.transitionDuration },
            targets: phase.targets.map(target => ({ type: target.type, operator: target.operator, value: target.value })),
        })),
    }
}

const profilesApp = Vue.createApp({
    data() {
        return {
            loading: true,
            loadError: '',
            saving: false,
            message: '',
            success: false,
            warnings: [],
            selected: -1,
            profiles: [],
            maxProfiles: 8,
            maxPhases: 8,
            maxTargets: 4,
            profilingAvailable: true,
            pressureSensor: true,
            transitions: TRANSITIONS,
            chartWidth: CHART_WIDTH,
            chartHeight: CHART_HEIGHT,
        }
    },
    methods: {
        async load() {
            this.loading = true
            this.loadError = ''
            this.message = ''
            this.warnings = []

            try {
                const response = await fetch('/profiles')

                if (!response.ok) {
                    throw new Error('HTTP ' + response.status)
                }

                const data = await response.json()
                this.selected = data.selected
                this.profiles = data.profiles.map(profile => fromStored(profile, []))
                this.maxProfiles = data.maxProfiles
                this.maxPhases = data.maxPhases
                this.maxTargets = data.maxTargets
                this.profilingAvailable = data.profilingAvailable
                this.pressureSensor = data.pressureSensor
            } catch (error) {
                this.loadError = 'Could not load the profiles: ' + error.message
            } finally {
                this.loading = false
            }
        },
        async save() {
            this.saving = true
            this.message = ''

            try {
                const response = await fetch('/profiles', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ selected: this.selected, profiles: this.profiles.map(toStored) }),
                })
                const result = await response.json()
                this.success = result.success
                this.message = result.message
            } catch (error) {
                this.success = false
                this.message = 'Saving failed: ' + error.message
            } finally {
                this.saving = false
            }
        },
        addProfile() {
            this.profiles.push({ label: 'New profile', phases: [newPhase('Pre-infusion', 'pressure', 3, 8), newPhase('Extraction', 'pressure', 9, 25)] })
        },
        duplicateProfile(index) {
            const copy = JSON.parse(JSON.stringify(this.profiles[index]))
            copy.label = (copy.label + ' copy').substring(0, NAME_LENGTH)
            this.profiles.splice(index + 1, 0, copy)

            if (this.selected > index) {
                this.selected++
            }
        },
        deleteProfile(index) {
            if (!confirm('Delete profile "' + this.profiles[index].label + '"?')) {
                return
            }

            this.profiles.splice(index, 1)

            // keep the selection on the same profile, or fall back to simple mode if it was deleted
            if (this.selected === index) {
                this.selected = -1
            } else if (this.selected > index) {
                this.selected--
            }
        },
        exportProfile(profile) {
            const blob = new Blob([JSON.stringify(toStored(profile), null, 2)], { type: 'application/json' })
            const link = document.createElement('a')
            link.href = URL.createObjectURL(blob)
            link.download = profile.label.replace(/[^a-z0-9-_]+/gi, '_') + '.json'
            link.click()
            URL.revokeObjectURL(link.href)
        },
        // accepts a single GaggiMate/CleverCoffee profile, a list of profiles or a full /profiles document
        async importProfiles(event) {
            const file = event.target.files[0]
            event.target.value = ''

            if (!file) {
                return
            }

            this.message = ''
            this.warnings = []

            try {
                const json = JSON.parse(await file.text())
                const list = Array.isArray(json) ? json : json.profiles || [json]
                const warnings = []
                let imported = 0

                for (const profile of list) {
                    if (!profile.phases || profile.phases.length === 0) {
                        warnings.push((profile.label || 'Profile') + ': no phases, skipped')
                        continue
                    }

                    if (this.profiles.length >= this.maxProfiles) {
                        warnings.push('Profile limit (' + this.maxProfiles + ') reached, remaining profiles skipped')
                        break
                    }

                    const converted = fromStored(profile, warnings)

                    if (converted.phases.length > this.maxPhases) {
                        warnings.push(converted.label + ': only the first ' + this.maxPhases + ' phases were kept')
                        converted.phases = converted.phases.slice(0, this.maxPhases)
                    }

                    this.profiles.push(converted)
                    imported++
                }

                this.warnings = warnings
                this.success = imported > 0
                this.message = imported > 0 ? imported + ' profile(s) imported, review and save them' : 'Nothing imported'
            } catch (error) {
                this.success = false
                this.message = 'Import failed: ' + error.message
            }
        },
        addPhase(profile) {
            const last = profile.phases[profile.phases.length - 1]
            profile.phases.push(newPhase('Phase ' + (profile.phases.length + 1), last ? last.mode : 'pressure', last ? last.value : 9, 10))
        },
        movePhase(profile, index, direction) {
            const [phase] = profile.phases.splice(index, 1)
            profile.phases.splice(index + direction, 0, phase)
        },
        addTarget(phase) {
            phase.targets.push({ type: 'volumetric', operator: 'gte', value: 36 })
        },
        needsSensor(profile) {
            return profile.phases.some(phase => phase.mode === 'pressure' || phase.targets.some(target => target.type === 'pressure'))
        },
        totalDuration(profile) {
            return profile.phases.reduce((sum, phase) => sum + (Number(phase.duration) || 0), 0).toFixed(1)
        },
        chartY(bar) {
            return CHART_HEIGHT - (Math.min(bar, CHART_MAX_BAR) / CHART_MAX_BAR) * (CHART_HEIGHT - 4) - 2
        },
        // nominal setpoint curves (early exits ignored): pressure in bar, pump power scaled to 0-100 % = 0-12 on the same axis
        chartSegments(profile) {
            const total = Number(this.totalDuration(profile))

            if (total <= 0) {
                return []
            }

            const xScale = CHART_WIDTH / total
            const segments = []
            let time = 0
            let previous = { mode: null, value: 0 }

            for (const phase of profile.phases) {
                const duration = Number(phase.duration) || 0
                const target = Number(phase.value) || 0
                // like the firmware: continue from the previous setpoint in the same mode, otherwise the start value is unknown (shown from 0)
                const start = previous.mode === phase.mode ? previous.value : 0
                let ramp = Math.min(Number(phase.transitionDuration) || 0, duration)
                if (ramp <= 0) ramp = duration
                const steps = phase.transition === 'instant' || ramp <= 0 ? 0 : 20
                const scale = phase.mode === 'power' ? CHART_MAX_BAR / 100 : 1
                const points = [[time, steps ? start : target]]

                for (let i = 1; i <= steps; i++) {
                    const t = (i / steps) * ramp
                    points.push([time + t, start + (target - start) * transitionCurve(phase.transition, t / ramp)])
                }

                points.push([time + duration, target])
                segments.push({
                    mode: phase.mode,
                    points: points.map(([t, v]) => (t * xScale).toFixed(1) + ',' + this.chartY(v * scale).toFixed(1)).join(' '),
                })

                time += duration
                previous = { mode: phase.mode, value: target }
            }

            return segments
        },
    },
    mounted() {
        this.load()
    },
})

profilesApp.mount('#content')
