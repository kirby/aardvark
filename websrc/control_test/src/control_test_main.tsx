import * as React from 'react';
import  * as ReactDOM from 'react-dom';

import { AvTransform } from 'common/aardvark-react/aardvark_transform';
import bind from 'bind-decorator';
import { AvSlider } from 'common/aardvark-react/aardvark_slider';
import { AvGrabbable } from 'common/aardvark-react/aardvark_grabbable';
import { AvSphereHandle } from 'common/aardvark-react/aardvark_handles';
import { AvModel } from 'common/aardvark-react/aardvark_model';
import { AvPanel } from 'common/aardvark-react/aardvark_panel';
import { AvPanelAnchor } from 'common/aardvark-react/aardvark_panelanchor';
import { AvTranslateControl } from 'common/aardvark-react/aardvark_translate_control';


interface ControlTestState
{
	sliderValue: number;
	translateValue: [ number, number, number ];
}


class ControlTest extends React.Component< {}, ControlTestState >
{
	constructor( props: any )
	{
		super( props );
		this.state = 
		{ 
			sliderValue: 0,
			translateValue: [ 0, 0, 0 ],
		};
	}

	@bind onSetSlider( newValue: number[] )
	{
		this.setState( { sliderValue: newValue[0] } );
	}

	@bind onSetTranslate( newValue: number[] )
	{
		this.setState( { translateValue: ( newValue as [number, number, number ]) } );
	}

	public render()
	{
		return (
			<div className="FullPage" >
				<AvGrabbable preserveDropTransform={true}>
					<AvTransform uniformScale={0.1}>
						<AvModel uri="https://aardvark.install/models/sphere/sphere.glb"/>
					</AvTransform>
					<AvSphereHandle radius={0.1} />

					<AvPanel interactive={ false } >
						<div className="ControlList">
							<div className="SliderContainer">
								<div className="SliderLabel">{ this.state.sliderValue.toFixed( 2 ) }</div>
								<div className="SliderControl">
									<AvPanelAnchor>
										<AvSlider rangeX={ 0.7 } onSetValue={ this.onSetSlider }
											modelUri="https://aardvark.install/models/gear.glb"/>
									</AvPanelAnchor>
								</div>
							</div>

							<div className="TranslateContainer">
								<div className="TranslateLabel">
									[ 
										{ this.state.translateValue[0] },
										{ this.state.translateValue[1] },
										{ this.state.translateValue[2] }
									]
								</div>
								<div className="TranslateControl">
									<AvPanelAnchor>
										<AvTranslateControl onSetValue={ this.onSetTranslate } />
									</AvPanelAnchor>
								</div>
							</div>
						</div>
					</AvPanel>
				</AvGrabbable>
			</div>
		)
	}
}

ReactDOM.render( <ControlTest/>, document.getElementById( "root" ) );